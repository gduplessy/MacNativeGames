"""gen_cpu_bridge.py: the generated guest -> host NVN bridge for the jit CPU backend (no game data, no compiler).

The native test (games/totk/tests/cpu_bridge, Windows x64) runs every marshaller against an AAPCS64 model through
dynarmic; this keeps the committed nvn_bridge.c in step with the NVN sources on every platform, and pins the
classification rules the marshallers depend on.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(ROOT/'games/totk/tools'))
import gen_cpu_bridge as gen   # noqa: E402

TYPES = gen.nvn_types((NVN/'include/nvn.h').read_text(encoding='utf-8'))


class Classification(unittest.TestCase):
    def test_scalars_and_pointers(self):
        for ctype, cls in [('int', 'I32'), ('uint32_t', 'I32'), ('NVNformat', 'I32'), ('NVNboolean', 'U8'),
                           ('size_t', 'I64'), ('NVNbufferAddress', 'I64'), ('const NVNdevice *', 'I64'),
                           ('PFNNVNDEBUGCALLBACKPROC', 'I64'), ('NVNnativeWindow', 'I64'), ('float', 'F32'), ('void', 'V')]:
            with self.subTest(ctype=ctype):
                self.assertEqual(gen.classify(ctype, TYPES), cls)

    def test_structs_by_value(self):
        self.assertEqual(gen.classify('NVNseparateTextureHandle', TYPES), 'S8')   # { uint64_t value; }: one x register
        self.assertEqual(gen.classify('NVNmemoryPool', TYPES), 'MEM:256')   # over 16 bytes: through x8
        with self.assertRaises(gen.BridgeError):
            gen.sig_classes('void', ['NVNmemoryPool'], TYPES, 'by-value struct argument')

    def test_unknown_type_fails(self):
        with self.assertRaises(gen.BridgeError):
            gen.classify('NVNnotAType', TYPES)

    def test_parameter_names(self):
        self.assertEqual(gen.drop_name('const NVNdevice *d'), 'const NVNdevice *')
        self.assertEqual(gen.drop_name('float c[4]'), 'float *')
        self.assertEqual(gen.drop_name('NVNboolean e'), 'NVNboolean')
        self.assertEqual(gen.drop_name('int'), 'int')

    def test_no_stack_arguments(self):
        gen.sig_classes('void', ['int'] * 8 + ['float'] * 8, TYPES, 'eight of each')   # fits in x0..x7 and v0..v7
        with self.assertRaises(gen.BridgeError):
            gen.sig_classes('void', ['int'] * 9, TYPES, 'nine integers')

    def test_separate_register_counts(self):
        # AAPCS64: the float takes s0 although it is the second argument; the next integer takes x1
        line = gen.emit_marshaller('V', ('I64', 'F32', 'I32'))
        self.assertIn('cpu_arg_i64(r, 0), cpu_arg_f32(r, 0), cpu_arg_i32(r, 1)', line)
        # the negative control numbers by position (the Windows x64 rule), which the native test must reject
        bad = gen.emit_marshaller('V', ('I64', 'F32', 'I32'), positional=True)
        self.assertIn('cpu_arg_i64(r, 0), cpu_arg_f32(r, 1), cpu_arg_i32(r, 2)', bad)

    def test_results(self):
        self.assertIn('*(cpu_m256_t *)(uintptr_t)r->x[8] =', gen.emit_marshaller('MEM:256', ('I64',)))
        self.assertIn('cpu_ret_i64(r, (uint32_t)', gen.emit_marshaller('I32', ('I64',)))
        self.assertIn('cpu_ret_f32(r,', gen.emit_marshaller('F32', ('I64',)))


class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = gen.build(NVN)

    def test_committed_bridge_is_current(self):
        text, _ = gen.render(self.model)
        self.assertEqual(gen.DEFAULT_OUTPUT.read_text(encoding='utf-8'), text,
                         'runtime/cpu/nvn_bridge.c is stale: run python games/totk/tools/gen_cpu_bridge.py games/totk/third_party/d2r_gfx/gfx/nvn')

    def test_slots_follow_names_txt(self):
        names = [l.strip() for l in (NVN/'names.txt').read_text(encoding='utf-8').splitlines() if l.strip()]
        self.assertEqual([s[0] for s in self.model['slots']], names)

    def test_implementation_wins_over_prototype(self):
        # nvn_protos.h has these returning NVNmemoryPool by value; the implementations return a pointer, as NVN does
        mismatched = {m[0]: m for m in self.model['mismatches']}
        for name in ('nvnTextureGetMemoryPool', 'nvnTextureBuilderGetMemoryPool'):
            with self.subTest(name=name):
                self.assertIn(name, mismatched)
                slot = next(s for s in self.model['slots'] if s[0] == name)
                self.assertEqual((slot[1], slot[2], slot[3]), ('I64', ('I64',), 'impl'))

    def test_unknown_names_are_not_implemented(self):
        impls = gen.impl_signatures(sorted(p for p in NVN.iterdir() if p.suffix in ('.c', '.m')))
        self.assertTrue(self.model['unknown'])
        for name in self.model['unknown']:
            self.assertNotIn(name, impls)

    def test_extras_have_bridges(self):
        names = {e[0] for e in self.model['extras']}
        self.assertIn('nvnCommandBufferBindTexturesDeferred', names)


if __name__ == '__main__':
    unittest.main()


RUNTIME = ROOT/'games/totk/runtime'


class StructsAndPortability(unittest.TestCase):
    def types(self, body):
        return {'T': ('struct', body), **TYPES}

    def test_struct_size_decides(self):
        # time_host.c's CalendarTime: 8 bytes of int16/int8 members comes back in x0, not through x8
        self.assertEqual(gen.classify('T', self.types('int16_t year; int8_t month, day, hour, minute, second, pad;')), 'S8')
        self.assertEqual(gen.classify('T', self.types('uint64_t a; uint32_t b[5];')), 'MEM:32')
        for body in ('uint32_t a, b, c;', 'uint8_t a;', 'float x, y;'):   # x0/x1 pairs, small structs, HFAs: not yet
            with self.subTest(body=body), self.assertRaises(gen.BridgeError):
                gen.classify('T', self.types(body))

    def test_long_is_refused(self):
        for t in ('long', 'unsigned long'):
            with self.subTest(t=t), self.assertRaises(gen.BridgeError):
                gen.classify(t, TYPES)


class HleImports(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = gen.build_hle(RUNTIME, NVN, TYPES)
        cls.by_symbol = {e[0]: e for e in cls.entries}

    def test_committed_bridge_is_current(self):
        text, _ = gen.render_hle(self.entries)
        self.assertEqual(gen.HLE_OUTPUT.read_text(encoding='utf-8'), text,
                         'runtime/cpu/hle_bridge.c is stale: run python games/totk/tools/gen_cpu_bridge.py games/totk/third_party/d2r_gfx/gfx/nvn')

    def test_anchors(self):
        cases = {
            '_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE': ('S8', ('I64',)),          # struct result in x0
            '_ZN2nv18InitializeGraphicsEPvm': ('V', ('I64', 'I64')),                          # size_t, not unsigned long
            '_ZN2nn2oe24SetExpectedVolumeBalanceEff': ('V', ('F32', 'F32')),                  # floats in s0, s1
            'nvnBootstrapLoader': ('I64', ('I64',)),                                          # HLE_FORCE
            'nnMain': ('V', ()),                                                              # runtime_observer
            '_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm': ('I32', ('I64', 'I64')),          # GAME_BOTW only
            '_ZN5movie7DecoderD1Ev': ('V', ('I64',)),                                         # movie_host.c alias
        }
        for symbol, sig in cases.items():
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, self.by_symbol)
                self.assertEqual(self.by_symbol[symbol][2:], sig)

    def test_every_override_entry_is_bridged(self):
        # each {"symbol", fn} pair of an override table resolves; only guest functions the host calls stay out
        for path in sorted(RUNTIME.glob('*.[cm]')):
            text = gen.strip_comments(path.read_text(encoding='utf-8'))
            if 'runtime_' not in text or '_override(const char *name)' not in text.replace('  ', ' '):
                continue
            for symbol, fn in gen.table_pairs(text):
                with self.subTest(file=path.name, symbol=symbol):
                    self.assertIn(symbol, self.by_symbol)


class GuestCalls(unittest.TestCase):
    """Host code calling guest functions goes through GUEST_CALL (cpu_backend.h), so the jit backend can run them."""
    def test_no_direct_guest_calls_left(self):
        services = (RUNTIME/'services.c').read_text(encoding='utf-8')
        self.assertNotRegex(services, r'\)\s*runtime_symbol\([^)]*\)\s*\)\s*\(', 'cast-and-call of a guest symbol in services.c')
        movie = (RUNTIME/'movie_host.c').read_text(encoding='utf-8')
        self.assertNotRegex(movie, r'\(\(\w+_fn\)G\.orig\[', 'direct call of an original movie entry')
        self.assertNotRegex(movie, r'(?<![&\w.])G\.(signal_event|buf_\w+|md_\w+)\(', 'direct call of a MovieGuest helper')

    def test_native_expansion_is_the_old_call(self):
        header = (RUNTIME/'cpu/cpu_backend.h').read_text(encoding='utf-8')
        self.assertIn('#define GUEST_CALL(ret, fn_type, fn, ...) (((fn_type)(fn))(__VA_ARGS__))', header)
        self.assertIn('#define GUEST_CALL0(ret, fn_type, fn) (((fn_type)(fn))())', header)
