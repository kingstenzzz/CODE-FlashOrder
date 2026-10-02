import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'doc'))
import unittest

from HG import PreferenceDisagreementSorter, disagreement, logical_delta, preference_matrix


class StandaloneTests(unittest.TestCase):
    def test_formula(self):
        pref = [[0, 5, 5], [0, 0, 3], [0, 2, 0]]
        self.assertAlmostEqual(disagreement(pref), 4 / 15)
        self.assertEqual(logical_delta([[0, 2], [2, 0]], 4), 1)

    def test_sort_is_permutation(self):
        rows = [[0, 1, 2, 3], [0, 1, 2, 3], [1, 0, 2, 3], [1, 0, 2, 3]]
        result = PreferenceDisagreementSorter(4, 0, gamma=0.6, k_d=4).sort(rows)
        self.assertEqual(sorted(result.order), [0, 1, 2, 3])
        self.assertEqual(result.delta, 2)


if __name__ == '__main__':
    unittest.main()
