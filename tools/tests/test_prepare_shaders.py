"""tools/prepare_shaders.py names variants as the runtime does (src/gfx/vk/vk_shader.c answers_json)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import prepare_shaders as ps  # noqa: E402


class Names(unittest.TestCase):
    def test_runtime_text(self):
        a = {'attr': {'0': 'float'}, 'fout': {}, 'img': {}, 'tex': {'10': '2d', '9': 'cube'}}
        self.assertEqual(ps.runtime_answers(a), '"tex": {"9": "cube", "10": "2d"}, "img": {}, "attr": {"0": "float"}, "fout": {}')
        self.assertEqual(ps.runtime_answers({'img': {'2': {'type': '2d', 'fmt': 'r32ui'}, '1': {'type': 'buffer'}}}),
                         '"tex": {}, "img": {"1": {"type": "buffer"}, "2": {"type": "2d", "fmt": "r32ui"}}, "attr": {}, "fout": {}')

    def test_name(self):
        name = ps.variant_name('k', {'attr': {'0': 'float'}})
        self.assertRegex(name, r'^k-[0-9a-f]{12}$')
        self.assertNotEqual(name, ps.variant_name('k', {'attr': {'0': 'uint'}}))


if __name__ == '__main__':
    unittest.main()
