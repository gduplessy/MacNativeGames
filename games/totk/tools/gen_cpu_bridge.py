#!/usr/bin/env python3
"""Generate the guest -> host bridges: NVN and the HLE imports (runtime/cpu/cpu_backend.h, port plan 9.1).

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

The HLE imports (runtime_*_override, runtime_observer, HLE_FORCE) get the same treatment in hle_bridge.c, keyed by the
import's symbol and typed by the host function's own C definition.

Usage: gen_cpu_bridge.py <gfx/nvn dir> [--runtime dir] [-o nvn_bridge.c] [--hle-output hle_bridge.c] [--probes probes.c]
                         [--check] [--negative]
"""
import argparse, re, sys
from pathlib import Path

# ---- C types -> argument classes ----
# I32: 32-bit integer or enum (wN), U8: 8-bit (wN low byte), I64: 64-bit integer or pointer (xN), F32 / F64: sN / dN,
# S8: an 8-byte struct, passed and returned like I64. Results also: V (void), MEM:<size> (a struct over 16 bytes, through x8).
CTYPE = {'I32': 'int32_t', 'U8': 'uint8_t', 'I64': 'uint64_t', 'F32': 'float', 'F64': 'double', 'S8': 'cpu_s8_t'}
SCALARS = {
    'int': 'I32', 'unsigned': 'I32', 'unsigned int': 'I32', 'int32_t': 'I32', 'uint32_t': 'I32',
    'uint8_t': 'U8', 'int8_t': 'U8', 'bool': 'U8', '_Bool': 'U8', 'char': 'U8', 'unsigned char': 'U8', 'signed char': 'U8',
    'uint64_t': 'I64', 'int64_t': 'I64', 'size_t': 'I64', 'ptrdiff_t': 'I64', 'intptr_t': 'I64', 'uintptr_t': 'I64',
    'uint16_t': 'I32', 'int16_t': 'I32', 'short': 'I32', 'unsigned short': 'I32',   # wN; only the low bits are read
    'float': 'F32', 'double': 'F64', 'void': 'V',
}
# 64-bit on the LP64 hosts the runtime was written for, 32-bit on Windows (LLP64): a host signature must not use it
LLP64_UNSAFE = {'long', 'unsigned long', 'long int', 'unsigned long int', 'signed long'}


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
    if t in LLP64_UNSAFE:
        raise BridgeError(f'{ctype!r} is 64-bit on macOS but 32-bit on Windows: use int64_t / uint64_t')
    if t in SCALARS:
        return SCALARS[t]
    if depth > 8 or t not in types:
        raise BridgeError(f'unknown type {ctype!r}')
    v = types[t]
    if isinstance(v, tuple):   # struct by value: AAPCS64 and Windows x64 both go by size (and by float-only members)
        size, _, floats = struct_layout(v[1], types, t)
        if floats:
            raise BridgeError(f'{t}: a float-only struct by value (AAPCS64 HFA, v registers) is not supported')
        if size == 8:
            return 'S8'    # x0 / one x register; RAX / one register on Windows x64
        if size > 16:
            return f'MEM:{size}'   # through x8; hidden pointer on Windows x64
        raise BridgeError(f'{t}: a {size}-byte struct by value is not supported (AAPCS64 x0/x1, Windows x64 differs by size)')
    if v in CTYPE or v == 'V':
        return v
    return classify(v, types, depth + 1)


SIZES = {'I32': 4, 'U8': 1, 'I64': 8, 'F32': 4, 'F64': 8}
WIDTH = {'int8_t': 1, 'uint8_t': 1, 'char': 1, 'unsigned char': 1, 'signed char': 1, 'bool': 1, '_Bool': 1, 'NVNboolean': 1,
         'int16_t': 2, 'uint16_t': 2, 'short': 2, 'unsigned short': 2}


def struct_layout(body, types, name, depth=0):
    """(size, alignment, float-only) of a struct with natural alignment, from its member declarations."""
    off, align, floats, n = 0, 1, True, 0
    for decl in (x.strip() for x in strip_comments(body).split(';')):
        if not decl:
            continue
        m = re.fullmatch(r'((?:const\s+|unsigned\s+|signed\s+)*[\w]+(?:\s*\*+)?)\s+(.+)', decl)
        if not m:
            raise BridgeError(f'{name}: cannot read member {decl!r}')
        base, names = m.group(1).strip(), split_top(m.group(2))
        for nm in names:
            arr = 1
            for dim in re.findall(r'\[([^\]]+)\]', nm):
                arr *= int(dim, 0)
            ptr = '*' in base or nm.lstrip().startswith('*')
            t = re.sub(r'\s+', ' ', base.replace('const ', '')).strip()
            if ptr:
                size = al = 8; fl = False
            elif t in WIDTH:
                size = al = WIDTH[t]; fl = False
            elif t in types and isinstance(types[t], tuple):
                size, al, fl = struct_layout(types[t][1], types, t, depth + 1)
            else:
                c = classify(t, types, depth + 1)
                if c not in SIZES:
                    raise BridgeError(f'{name}: member type {t!r} has no size')
                size = al = SIZES[c]; fl = c in ('F32', 'F64')
            off = (off + al - 1) // al * al + size * arr
            align = max(align, al); floats &= fl; n += 1
    return (off + align - 1) // align * align, align, floats and 1 <= n <= 4


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
    if re.search(r'\(\s*\*', p):   # function pointer: 'void (*cb)(int)' -> 'void (*)(int)'
        return re.sub(r'\(\s*\*\s*\w*\s*\)', '(*)', p, count=1)
    m = re.fullmatch(r'(.*?)\s*\b(\w+)\s*(\[[^\]]*\])?', p)
    if not m:
        raise BridgeError(f'cannot parse parameter {param!r}')
    head, name, arr = m.group(1).strip(), m.group(2), m.group(3)
    if head in ('unsigned', 'signed', 'const unsigned', 'const signed') and name not in ('int', 'char', 'short', 'long'):
        return head   # 'unsigned flags': the type is plain unsigned
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
    return 'br_' + r.replace('MEM:', 'M') + '__' + ('_'.join(args) if args else 'void')


def mem_type(r):
    return f'cpu_m{r[4:]}_t'


def type_decls(sigs):
    """Host types the marshallers use for structs by value: only their size and alignment matter to either ABI."""
    out = ['typedef struct { uint64_t v; } cpu_s8_t;   /* an 8-byte struct: one x register, RAX on Windows x64 */']
    for size in sorted({int(r[4:]) for r, _ in sigs if r.startswith('MEM:')}):
        out.append(f'typedef struct {{ _Alignas(16) uint8_t b[{size}]; }} cpu_m{size}_t;   /* through x8; a hidden pointer on Windows x64 */')
    return out


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
    rtype = 'void' if r == 'V' else mem_type(r) if r.startswith('MEM:') else CTYPE[r]
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
    out = ['// Generated by games/totk/tools/gen_cpu_bridge.py from names.txt, nvn_protos.h and the NVN implementations;',
           '// do not edit. Guest -> host NVN calls for the jit CPU backend (runtime/cpu/cpu_backend.h).',
           f'// {len(model["slots"])} slots ({sum(1 for s in model["slots"] if s[3] == "impl")} implemented, '
           f'{sum(1 for s in model["slots"] if s[3] == "proto")} from nvn_protos.h only, {len(model["unknown"])} without a signature), '
           f'{len(model["extras"])} extras, {len(sigs)} marshallers.',
           '#include <stdint.h>',
           '#include "cpu_backend.h"']
    out += [''] + type_decls(sigs) + ['']
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
           '#include "cpu_backend.h"', *type_decls(sigs),
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
        rtype = 'void' if r == 'V' else mem_type(r) if r.startswith('MEM:') else CTYPE[r]
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


# ---- HLE imports: the runtime's host replacements for guest imports (runtime_*_override, runtime_observer, HLE_FORCE) ----
# loader.c binds a guest import to a host function when one of these tables names it. On an ARM64 host the import's GOT
# slot then holds the host function (behind hle_stack.S); on an x86-64 host it holds a host-entry stub, and the marshaller
# chosen here by the import's name moves the arguments. The signature is the host function's own C definition.
HLE_OUTPUT = DEFAULT_OUTPUT.with_name('hle_bridge.c')
SYMBOL = re.compile(r'(_Z\w+|nn\w+|nvn\w+)')
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do'}


def c_functions(text):
    """name -> {(ret, (param types))} for every function definition in a comment-stripped C / Objective-C file."""
    found = {}
    rx = re.compile(r'(?:^|[;}\n])\s*((?:(?:static|inline|extern|__attribute__\s*\(\([^()]*(?:\([^()]*\))*[^()]*\)\))\s+)*)'
                    r'((?:const\s+|unsigned\s+|signed\s+|struct\s+|enum\s+)*\w+(?:\s*\*+\s*|\s+))(\w+)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*\{')
    for m in rx.finditer(text):
        ret, name, params = m.group(2).strip(), m.group(3), m.group(4)
        if name in KEYWORDS or ret in KEYWORDS or ret in ('return', 'else'):
            continue
        plist = tuple(drop_name(p) for p in split_top(params) if p.strip() not in ('', 'void'))
        found.setdefault(name, set()).add((re.sub(r'\s+', ' ', ret), plist))
    return found


def c_typedefs(text):
    """Local typedefs of a runtime file, in the form nvn_types returns."""
    return nvn_types(text)


def table_pairs(text):
    """(symbol, host function) pairs an override function hands out: {"sym", fn} entries, strcmp(name,"sym") returns,
    and movie_host.c's parallel NAMES[] / ENTRIES[] arrays."""
    pairs = []
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(?:\(\s*void\s*\*\s*\)\s*)?(\w+)', text):
        pairs.append((m.group(1), m.group(2)))
    for m in re.finditer(r'strcmp\(\s*name\s*,\s*"([^"]+)"\s*\)\s*\)\s*return\s+(?:[^;?]*\?\s*NULL\s*:\s*)?(?:\(\s*void\s*\*\s*\)\s*)?(\w+)\s*;', text):
        pairs.append((m.group(1), m.group(2)))
    names = re.search(r'\bNAMES\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    entries = re.search(r'\bENTRIES\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    if names and entries:
        ns = re.findall(r'"([^"]+)"', names.group(1))
        es = [e.strip().replace('(void *)', '').strip() for e in split_top(entries.group(1))]
        if len(ns) != len(es):
            raise BridgeError(f'NAMES[] has {len(ns)} entries, ENTRIES[] {len(es)}')
        pairs += list(zip(ns, es))
    return [(s, f) for s, f in pairs if SYMBOL.fullmatch(s)]


def hle_force_entries(text):
    """HLE_FORCE(mangled, reason, ret, cname, params...) declarations (gfx/host/hle.h)."""
    out = []
    for m in re.finditer(r'^\s*HLE_FORCE\(', text, re.M):
        i, depth = m.end(), 1
        while depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0); i += 1
        parts = split_top(text[m.end():i - 1])
        if len(parts) < 4 or not parts[0].startswith('"'):
            continue   # the #define
        params = tuple(drop_name(p) for p in parts[4:] if p.strip() not in ('', 'void'))
        out.append((parts[0].strip('"'), parts[3], parts[2], params))
    return out


def build_hle(runtime_dir, nvn_dir, types):
    runtime_dir, nvn_dir = Path(runtime_dir), Path(nvn_dir)
    sources = sorted(p for p in runtime_dir.iterdir() if p.suffix in ('.c', '.m'))
    by_symbol, problems = {}, []
    all_types = dict(types)
    texts = {p: strip_comments(p.read_text(encoding='utf-8')) for p in sources}
    for t in texts.values():
        for k, v in c_typedefs(t).items():
            all_types.setdefault(k, v)

    def add(symbol, fn, ret, params, where):
        try:
            r, a = sig_classes(ret, list(params), all_types, f'{symbol} -> {where}')
        except BridgeError as e:
            problems.append(f'{where} ({symbol}): {e}'); return
        prev = by_symbol.get(symbol)
        if prev and (prev[2], prev[3]) != (r, a):
            problems.append(f'{symbol}: {prev[1]} is {sig_key(prev[2], prev[3])} but {where} is {sig_key(r, a)}')
            return
        if not prev:
            by_symbol[symbol] = (symbol, where, r, a)

    for path, text in texts.items():
        if not re.search(r'void\s*\*\s*runtime_\w*(override|observer)\s*\(\s*const\s+char\s*\*\s*name\s*\)', text):
            continue
        funcs = c_functions(text)
        for symbol, fn in table_pairs(text):
            if fn in ('NULL', '0'):
                continue
            if fn not in funcs:
                problems.append(f'{symbol}: host function {fn} not defined in {path.name}'); continue
            for ret, params in sorted(funcs[fn]):
                add(symbol, fn, ret, params, f'{path.name}:{fn}')
    for path in sorted(p for p in nvn_dir.iterdir() if p.suffix in ('.c', '.m')):
        for symbol, cname, ret, params in hle_force_entries(strip_comments(path.read_text(encoding='utf-8'))):
            add(symbol, cname, ret, params, f'{path.name}:{cname}')
    if problems:
        raise BridgeError('HLE imports:\n  ' + '\n  '.join(problems))
    return sorted(by_symbol.values())


def render_hle(entries, positional=False):
    sigs = sorted({(r, a) for _, _, r, a in entries})
    files = sorted({w.split(':')[0] for _, w, _, _ in entries})
    out = ['// Generated by games/totk/tools/gen_cpu_bridge.py from the runtime\'s import overrides (runtime_*_override,',
           '// runtime_observer) and HLE_FORCE; do not edit. Guest -> host calls for the jit CPU backend (cpu_backend.h).',
           f'// {len(entries)} imports from {len(files)} files, {len(sigs)} marshallers: {", ".join(files)}.',
           '#include <stdint.h>',
           '#include <string.h>',
           '#include "cpu_backend.h"', '',
           *type_decls(sigs), '']
    out += [emit_marshaller(r, a, positional) for r, a in sigs]
    out += ['', 'typedef struct { const char *symbol; cpu_bridge_fn bridge; const char *sig; const char *host; } hle_bridge_import;',
            '/* Sorted by symbol (strcmp order) for hle_bridge_for_import. host: the implementing file and function. */',
            f'const hle_bridge_import hle_bridge_imports[{len(entries)}] = {{']
    out += [f'    {{"{s}", {marshaller_name(r, a)}, "{sig_key(r, a)}", "{w}"}},' for s, w, r, a in entries]
    out += ['};', f'const unsigned hle_bridge_nimports = {len(entries)};', '',
            '/* The marshaller for a guest import the runtime replaces, or NULL if the import is not bridged. */',
            'cpu_bridge_fn hle_bridge_for_import(const char *symbol) {',
            '    unsigned lo = 0, hi = hle_bridge_nimports;',
            '    while (lo < hi) {',
            '        unsigned mid = (lo + hi) / 2; int c = strcmp(symbol, hle_bridge_imports[mid].symbol);',
            '        if (!c) return hle_bridge_imports[mid].bridge;',
            '        if (c < 0) hi = mid; else lo = mid + 1;',
            '    }',
            '    return 0;',
            '}', '']
    return '\n'.join(out) + '\n', sigs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('nvn_dir')
    ap.add_argument('--runtime', help='runtime directory for the HLE imports (default: games/totk/runtime)')
    ap.add_argument('-o', '--output', help=f'NVN bridge (default {DEFAULT_OUTPUT.name})')
    ap.add_argument('--hle-output', help=f'HLE import bridge (default {HLE_OUTPUT.name})')
    ap.add_argument('--probes')
    ap.add_argument('--check', action='store_true', help='fail if an output differs from what would be generated')
    ap.add_argument('--negative', action='store_true', help='tests only: emit a deliberately wrong (positional) register assignment')
    a = ap.parse_args(argv)
    if a.negative and (a.check or not a.output or not a.hle_output):
        print('gen_cpu_bridge: --negative needs -o and --hle-output and cannot be combined with --check', file=sys.stderr); return 1
    runtime_dir = Path(a.runtime) if a.runtime else DEFAULT_OUTPUT.parents[1]
    try:
        model = build(a.nvn_dir)
        types = nvn_types((Path(a.nvn_dir) / 'include' / 'nvn.h').read_text(encoding='utf-8'))
        hle = build_hle(runtime_dir, a.nvn_dir, types)
    except BridgeError as e:
        print(f'gen_cpu_bridge: {e}', file=sys.stderr); return 1
    text, sigs = render(model, a.negative)
    hle_text, hle_sigs = render_hle(hle, a.negative)
    outputs = [(Path(a.output) if a.output else DEFAULT_OUTPUT, text), (Path(a.hle_output) if a.hle_output else HLE_OUTPUT, hle_text)]
    for out, content in outputs:
        if a.check:
            if not out.exists() or out.read_text(encoding='utf-8') != content:
                print(f'gen_cpu_bridge: {out} is stale; rerun tools/gen_cpu_bridge.py', file=sys.stderr); return 1
        else:
            out.write_text(content, encoding='utf-8', newline='\n')
    if a.probes:
        Path(a.probes).write_text(render_probes(sorted(set(sigs) | set(hle_sigs))), encoding='utf-8', newline='\n')
    impl = sum(1 for s in model['slots'] if s[3] == 'impl')
    print(f'NVN: {len(model["slots"])} slots: {impl} implemented, {len(model["slots"]) - impl - len(model["unknown"])} from nvn_protos.h, '
          f'{len(model["unknown"])} without a signature; {len(model["extras"])} extras; {len(sigs)} marshallers; '
          f'{len(model["mismatches"])} prototype mismatches')
    print(f'HLE: {len(hle)} imports from {len({w.split(":")[0] for _, w, _, _ in hle})} files; {len(hle_sigs)} marshallers')
    return 0


if __name__ == '__main__':
    sys.exit(main())
