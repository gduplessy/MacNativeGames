#!/usr/bin/env python3
"""Generate the guest -> host bridge for NVN (runtime/cpu/cpu_backend.h, docs/windows-android-port-plan.md 9.1).

On an ARM64 host the game calls NVN through nvn_thunks.S: guest and host share AAPCS64, so the thunk only counts the call
and branches to the implementation. On an x86-64 host the guest calls a stub instead (svc #CPU_SVC_BRIDGE + i), and the
code generated here reads the AAPCS64 arguments out of the guest's register frame and calls the implementation with the
host's C ABI.

Signatures come from the implementations first (NVN / NVN_NOPROTO / NVN_EXTRA in the NVN sources: what the host
function really is, and so what an ARM64 call reaches), then from nvn_protos.h for names the runtime does not implement.
One marshaller is emitted per distinct signature class, and nvn_bridge_sig[i] follows names.txt (the nvn_slot order).

AAPCS64 as the bridge applies it: integer, pointer and 8-byte struct arguments take x0..x7 in order, float and double
take v0..v7 in their own order; no NVN function passes arguments on the stack (checked here). A 32-bit or narrower
argument uses only the low bits of its register. A struct result larger than 16 bytes is written through x8.

Usage: gen_cpu_bridge.py <gfx/nvn dir> [-o runtime/cpu/nvn_bridge.c] [--probes probes.c] [--check] [--negative]
"""
import argparse, re, sys
from pathlib import Path

# ---- C types -> argument classes ----
# I32: 32-bit integer or enum (wN), U8: 8-bit (wN low byte), I64: 64-bit integer or pointer (xN), F32 / F64: sN / dN,
# S8: struct holding one 64-bit integer, passed and returned like I64. Results also: V (void), MEM:<type> (through x8).
CTYPE = {'I32': 'int32_t', 'U8': 'uint8_t', 'I64': 'uint64_t', 'F32': 'float', 'F64': 'double', 'S8': 'cpu_s8_t'}
SCALARS = {
    'int': 'I32', 'unsigned': 'I32', 'unsigned int': 'I32', 'int32_t': 'I32', 'uint32_t': 'I32',
    'uint8_t': 'U8', 'int8_t': 'U8', 'bool': 'U8', '_Bool': 'U8',
    'uint64_t': 'I64', 'int64_t': 'I64', 'size_t': 'I64', 'ptrdiff_t': 'I64', 'intptr_t': 'I64', 'uintptr_t': 'I64',
    'float': 'F32', 'double': 'F64', 'void': 'V',
}


# runtime/cpu: built only for x86-64 hosts (the Makefile's runtime/*.c glob is not recursive; ios/project.yml excludes it)
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / 'runtime' / 'cpu' / 'nvn_bridge.c'


class BridgeError(Exception):
    pass


def strip_comments(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/', ' ', text, flags=re.S)


def nvn_types(header_text):
    """typedef name -> class or ('struct', body) from nvn.h."""
    text = strip_comments(header_text)
    types = {}
    for m in re.finditer(r'typedef\s+enum\s*\w*\s*\{.*?\}\s*(\w+)\s*;', text, re.S):
        types[m.group(1)] = 'I32'
    for m in re.finditer(r'typedef\s+struct\s*\w*\s*\{(.*?)\}\s*(\w+)\s*;', text, re.S):
        types[m.group(2)] = ('struct', m.group(1))
    for m in re.finditer(r'typedef\s+[^;{}()]*?\(\s*\*\s*(\w+)\s*\)\s*\(', text):
        types[m.group(1)] = 'I64'   # function pointer
    for m in re.finditer(r'typedef\s+([\w\s\*]+?)\s*\b(\w+)\s*;', text):
        src, name = m.group(1).strip(), m.group(2)
        if name not in types and not src.startswith(('struct', 'enum')):
            types[name] = src
    return types


def classify(ctype, types, depth=0):
    """Class of one C type as written in a declaration (parameter names already removed)."""
    t = re.sub(r'\s+', ' ', ctype.replace('const ', ' ').replace(' const', ' ').replace('volatile', ' ')).strip()
    if '*' in t or '[' in t:
        return 'I64'
    if t in SCALARS:
        return SCALARS[t]
    if depth > 8 or t not in types:
        raise BridgeError(f'unknown type {ctype!r}')
    v = types[t]
    if isinstance(v, tuple):   # struct by value
        members = [x.strip() for x in v[1].split(';') if x.strip()]
        if len(members) == 1 and re.fullmatch(r'(uint64_t|int64_t)\s+\w+', members[0]):
            return 'S8'
        return 'MEM:' + t
    if v in CTYPE or v == 'V':
        return v
    return classify(v, types, depth + 1)


def split_top(s):
    """Split on commas outside parentheses and brackets."""
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [x.strip() for x in out]


def drop_name(param):
    """'const NVNdevice *d' -> 'const NVNdevice *'; 'float c[4]' -> 'float *'; unnamed types stay."""
    p = param.strip()
    if p in ('void', ''):
        return p
    m = re.fullmatch(r'(.*?)\s*\b(\w+)\s*(\[[^\]]*\])?', p)
    if not m:
        raise BridgeError(f'cannot parse parameter {param!r}')
    head, name, arr = m.group(1).strip(), m.group(2), m.group(3)
    if not head or head in ('const', 'struct', 'enum', 'unsigned', 'signed') or name in SCALARS:
        return p   # no parameter name
    return head + (' *' if arr else '')


def proto_signatures(protos_text):
    sigs = {}
    for ret, name, args in re.findall(r'typedef\s+(.+?)\s*\(\*NVNT_(\w+)\)\((.*?)\);', protos_text):
        sigs[name] = (ret.strip(), [a for a in split_top(args) if a not in ('', 'void')])
    return sigs


def impl_signatures(sources):
    """name -> (kind, ret, [param types], file) from NVN(name, ret, params...) style declarations."""
    sigs = {}
    for path in sources:
        text = strip_comments(path.read_text(encoding='utf-8'))
        for m in re.finditer(r'^\s*(NVN|NVN_NOPROTO|NVN_EXTRA)\(', text, re.M):
            i, depth = m.end(), 1
            while depth:
                depth += {'(': 1, ')': -1}.get(text[i], 0); i += 1
            parts = split_top(text[m.end():i - 1])
            if len(parts) < 2 or not re.fullmatch(r'\w+', parts[0]):
                continue   # the #define itself
            name, ret = parts[0], parts[1]
            params = [drop_name(p) for p in parts[2:] if p.strip() not in ('', 'void')]
            if name in sigs and sigs[name][1:3] != (ret, params):
                raise BridgeError(f'{name}: two implementations with different signatures')
            sigs[name] = (m.group(1), ret, params, path.name)
    return sigs


def sig_classes(ret, params, types, where):
    r = classify(ret, types)
    args = [classify(p, types) for p in params]
    for a in args:
        if a == 'V' or a.startswith('MEM:'):
            raise BridgeError(f'{where}: by-value argument class {a} is not supported')
    if sum(a not in ('F32', 'F64') for a in args) > 8 or sum(a in ('F32', 'F64') for a in args) > 8:
        raise BridgeError(f'{where}: arguments would spill to the stack')
    if r.startswith('MEM:') and sum(a not in ('F32', 'F64') for a in args) > 8:
        raise BridgeError(f'{where}: no x register left')
    return r, tuple(args)


def sig_key(r, args):
    return r + ':' + ','.join(args)


def marshaller_name(r, args):
    return 'br_' + r.replace('MEM:', 'M_') + '__' + ('_'.join(args) if args else 'void')


def emit_marshaller(r, args, positional=False):
    """positional (--negative, tests only): number registers by argument position, as the Windows x64 ABI does, instead
    of keeping separate integer and float counts. A conformance test must reject the result."""
    ngrn = nsrn = 0
    call_args, proto_args = [], []
    for k, a in enumerate(args):
        if positional:
            ngrn = nsrn = k
        proto_args.append(CTYPE[a])
        if a in ('F32', 'F64'):
            call_args.append(f'cpu_arg_{a.lower()}(r, {nsrn})'); nsrn += 1
        elif a == 'S8':
            call_args.append(f'(cpu_s8_t){{r->x[{ngrn}]}}'); ngrn += 1
        elif a == 'U8':
            call_args.append(f'(uint8_t)r->x[{ngrn}]'); ngrn += 1
        else:
            call_args.append(f'cpu_arg_{a.lower()}(r, {ngrn})'); ngrn += 1
    rtype = 'void' if r == 'V' else r[4:] if r.startswith('MEM:') else CTYPE[r]
    fp = f'(({rtype} (*)({", ".join(proto_args) or "void"}))fn)({", ".join(call_args)})'
    if r == 'V':
        body = fp + ';'
    elif r in ('F32', 'F64'):
        body = f'cpu_ret_{r.lower()}(r, {fp});'
    elif r == 'S8':
        body = f'cpu_ret_i64(r, {fp}.v);'
    elif r.startswith('MEM:'):
        body = f'*({rtype} *)(uintptr_t)r->x[8] = {fp};'   # AAPCS64: x8 holds the result address
    elif r == 'I32':
        body = f'cpu_ret_i64(r, (uint32_t){fp});'
    else:
        body = f'cpu_ret_i64(r, (uint64_t){fp});'
    return f'static void {marshaller_name(r, args)}(cpu_regs *r, void *fn) {{ {body} }}'


def build(nvn_dir):
    nvn_dir = Path(nvn_dir)
    types = nvn_types((nvn_dir / 'include' / 'nvn.h').read_text(encoding='utf-8'))
    protos = proto_signatures((nvn_dir / 'nvn_protos.h').read_text(encoding='utf-8'))
    impls = impl_signatures(sorted(p for p in nvn_dir.iterdir() if p.suffix in ('.c', '.m')))
    names = [l.strip() for l in (nvn_dir / 'names.txt').read_text(encoding='utf-8').splitlines() if l.strip()]
    slots, extras, mismatches, unknown = [], [], [], []
    for name in names:
        if name in impls:
            kind, ret, params, src = impls[name]
            r, a = sig_classes(ret, params, types, name)
            if name in protos:
                pr, pa = sig_classes(protos[name][0], protos[name][1], types, name + ' (nvn_protos.h)')
                if (pr, pa) != (r, a):
                    mismatches.append((name, sig_key(pr, pa), sig_key(r, a), src))
            slots.append((name, r, a, 'impl'))
        elif name in protos:
            r, a = sig_classes(protos[name][0], protos[name][1], types, name)
            slots.append((name, r, a, 'proto'))
        else:
            slots.append((name, None, None, 'none')); unknown.append(name)
    for name, (kind, ret, params, src) in sorted(impls.items()):
        if kind == 'NVN_EXTRA':
            r, a = sig_classes(ret, params, types, name)
            extras.append((name, r, a))
        elif name not in names:
            raise BridgeError(f'{name} ({kind}, {src}) is implemented but not in names.txt')
    return dict(slots=slots, extras=extras, mismatches=mismatches, unknown=unknown)


def render(model, positional=False):
    sigs = sorted({(r, a) for _, r, a, _ in model['slots'] if r} | {(r, a) for _, r, a in model['extras']})
    mem_types = sorted({r[4:] for r, _ in sigs if r.startswith('MEM:')})
    out = ['// Generated by games/totk/tools/gen_cpu_bridge.py from names.txt, nvn_protos.h and the NVN implementations;',
           '// do not edit. Guest -> host NVN calls for the jit CPU backend (runtime/cpu/cpu_backend.h).',
           f'// {len(model["slots"])} slots ({sum(1 for s in model["slots"] if s[3] == "impl")} implemented, '
           f'{sum(1 for s in model["slots"] if s[3] == "proto")} from nvn_protos.h only, {len(model["unknown"])} without a signature), '
           f'{len(model["extras"])} extras, {len(sigs)} marshallers.',
           '#include <stdint.h>',
           '#include "cpu_backend.h"']
    if mem_types:
        out.append('#include "nvn.h"')
    out += ['', 'typedef struct { uint64_t v; } cpu_s8_t;   /* NVNseparateTextureHandle, NVNseparateSamplerHandle */', '']
    for mm in model['mismatches']:
        out.append(f'// {mm[0]}: nvn_protos.h says {mm[1]}, the implementation ({mm[3]}) is {mm[2]}; the bridge follows the implementation')
    if model['mismatches']:
        out.append('')
    out += [emit_marshaller(r, a, positional) for r, a in sigs]
    out += ['', f'#define NVN_BRIDGE_SLOTS {len(model["slots"])}',
            '/* Indexed like names.txt / nvn_slot; NULL: no known signature (the call can only reach the unimplemented-call report). */',
            'const cpu_bridge_fn nvn_bridge_sig[NVN_BRIDGE_SLOTS] = {']
    for i, (name, r, a, _) in enumerate(model['slots']):
        out.append(f'    {marshaller_name(r, a) if r else "0"},   /* {i} {name} */')
    out += ['};', 'const char *const nvn_bridge_sigstr[NVN_BRIDGE_SLOTS] = {']
    out += [f'    "{sig_key(r, a) if r else ""}",' for _, r, a, _ in model['slots']]
    out += ['};', '', '/* NVN_EXTRA entry points: returned by nvn_lookup directly, not in names.txt. */',
            'typedef struct { const char *name; cpu_bridge_fn bridge; const char *sig; } nvn_bridge_extra;',
            f'const nvn_bridge_extra nvn_bridge_extras[{max(1, len(model["extras"]))}] = {{']
    out += [f'    {{"{n}", {marshaller_name(r, a)}, "{sig_key(r, a)}"}},' for n, r, a in model['extras']] or ['    {0, 0, 0},']
    out += ['};', f'const unsigned nvn_bridge_nextras = {len(model["extras"])};', '']
    return '\n'.join(out) + '\n', sigs


def render_probes(sigs):
    """Test-only: one probe per marshaller with the host C signature; it records what it received and returns a value
    derived from it, so a test can check argument and result placement against an independent AAPCS64 model."""
    out = ['// Generated by gen_cpu_bridge.py --probes (tests only).', '#include <stdint.h>', '#include <string.h>',
           '#include "cpu_backend.h"', '#include "nvn.h"', 'typedef struct { uint64_t v; } cpu_s8_t;',
           'uint64_t probe_seen[16]; unsigned probe_nseen;',
           'static uint64_t probe_mix(void) { uint64_t h = 0x9e3779b97f4a7c15ULL; for (unsigned i = 0; i < probe_nseen; i++) h = (h ^ probe_seen[i]) * 0x100000001b3ULL; return h; }', '']
    names = []
    for r, a in sigs:
        pname = 'probe' + marshaller_name(r, a)[2:]
        params = ', '.join(f'{CTYPE[x]} a{k}' for k, x in enumerate(a)) or 'void'
        rec = []
        for k, x in enumerate(a):
            if x == 'F32':
                rec.append(f'{{ uint32_t b; memcpy(&b, &a{k}, 4); probe_seen[probe_nseen++] = b; }}')
            elif x == 'F64':
                rec.append(f'{{ uint64_t b; memcpy(&b, &a{k}, 8); probe_seen[probe_nseen++] = b; }}')
            elif x == 'S8':
                rec.append(f'probe_seen[probe_nseen++] = a{k}.v;')
            elif x == 'I32':
                rec.append(f'probe_seen[probe_nseen++] = (uint32_t)a{k};')
            else:
                rec.append(f'probe_seen[probe_nseen++] = (uint64_t)a{k};')
        rtype = 'void' if r == 'V' else r[4:] if r.startswith('MEM:') else CTYPE[r]
        if r == 'V':
            ret = ''
        elif r == 'F32':
            ret = 'float f = (float)(probe_mix() % 100000) / 7.0f; return f;'
        elif r == 'F64':
            ret = 'double d = (double)(probe_mix() % 100000) / 7.0; return d;'
        elif r == 'S8':
            ret = 'cpu_s8_t s = {probe_mix()}; return s;'
        elif r.startswith('MEM:'):
            ret = f'{rtype} m; uint64_t h = probe_mix(); for (size_t i = 0; i < sizeof m; i++) ((uint8_t *)&m)[i] = (uint8_t)(h >> (i % 8 * 8)) ^ (uint8_t)i; return m;'
        else:
            ret = f'return ({rtype})probe_mix();'
        out.append(f'static {rtype} {pname}({params}) {{ probe_nseen = 0; {" ".join(rec)} {ret} }}')
        names.append((sig_key(r, a), pname, marshaller_name(r, a)))
    out += ['', 'typedef struct { const char *sig; void *probe; } probe_entry;',
            f'const probe_entry probe_table[{len(names)}] = {{']
    out += [f'    {{"{s}", (void *){p}}},' for s, p, _ in names]
    out += ['};', f'const unsigned probe_count = {len(names)};', '']
    return '\n'.join(out) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('nvn_dir')
    ap.add_argument('-o', '--output')
    ap.add_argument('--probes')
    ap.add_argument('--check', action='store_true', help='fail if --output differs from what would be generated')
    ap.add_argument('--negative', action='store_true', help='tests only: emit a deliberately wrong (positional) register assignment')
    a = ap.parse_args(argv)
    try:
        model = build(a.nvn_dir)
    except BridgeError as e:
        print(f'gen_cpu_bridge: {e}', file=sys.stderr); return 1
    if a.negative and (a.check or not a.output):
        print('gen_cpu_bridge: --negative needs -o and cannot be combined with --check', file=sys.stderr); return 1
    text, sigs = render(model, a.negative)
    out = Path(a.output) if a.output else DEFAULT_OUTPUT
    if a.check:
        if not out.exists() or out.read_text(encoding='utf-8') != text:
            print(f'gen_cpu_bridge: {out} is stale; rerun tools/gen_cpu_bridge.py', file=sys.stderr); return 1
    else:
        out.write_text(text, encoding='utf-8', newline='\n')
    if a.probes:
        Path(a.probes).write_text(render_probes(sigs), encoding='utf-8', newline='\n')
    impl = sum(1 for s in model['slots'] if s[3] == 'impl')
    print(f'{len(model["slots"])} slots: {impl} implemented, {len(model["slots"]) - impl - len(model["unknown"])} from nvn_protos.h, '
          f'{len(model["unknown"])} without a signature; {len(model["extras"])} extras; {len(sigs)} marshallers; '
          f'{len(model["mismatches"])} prototype mismatches')
    return 0


if __name__ == '__main__':
    sys.exit(main())
