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
        self.assertEqual(gen.classify('NVNmemoryPool', TYPES), 'MEM:NVNmemoryPool')   # 256 bytes: through x8
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
        self.assertIn('*(NVNmemoryPool *)(uintptr_t)r->x[8] =', gen.emit_marshaller('MEM:NVNmemoryPool', ('I64',)))
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
