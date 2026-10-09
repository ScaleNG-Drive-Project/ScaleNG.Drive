import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from autonomous_test import parse_shadow_input_evidence, set_ini_key


class ShadowInputEvidenceTests(unittest.TestCase):
    def test_separates_real_class_records_from_real_heartbeat(self):
        lines = [
            "hooks: shadow-eval inputs REAL why=real mvClass=ENGINE_MV depthClass=ENGINE_DEPTH present=905",
            "hooks: shadow-eval real-inputs why=real useReal=1 mvTouchAge=1 depthTouchAge=2 present=905",
            "hooks: shadow-eval inputs ZERO why=depth-stale-present mvClass=PLACEHOLDER depthClass=PLACEHOLDER",
            "hooks: shadow-eval real-inputs why=depth-stale-present useReal=0 mvTouchAge=0 depthTouchAge=4 present=906",
        ]
        evidence = parse_shadow_input_evidence(lines)
        self.assertEqual(evidence["real_records"], 1)
        self.assertEqual(evidence["zero_records"], 1)
        self.assertEqual(evidence["engine_pair_records"], 1)
        self.assertEqual(evidence["real_ages"], [(1, 2)])

    def test_missing_age_evidence_stays_empty_not_zero(self):
        evidence = parse_shadow_input_evidence([
            "hooks: shadow-eval inputs REAL why=real mvClass=ENGINE_MV depthClass=ENGINE_DEPTH"
        ])
        self.assertEqual(evidence["real_records"], 1)
        self.assertEqual(evidence["real_ages"], [])


class IniOverrideTests(unittest.TestCase):
    def test_changes_only_target_key(self):
        original = "[ScaleNG]\r\nrealInputs=1\r\nshadowHandoff=1\r\n[bridge]\r\nhelper=0\r\n"
        changed = set_ini_key(original, "ScaleNG", "realInputs", "0")
        self.assertIn("realInputs=0\r\n", changed)
        self.assertIn("shadowHandoff=1\r\n[bridge]", changed)
        self.assertIn("helper=0\r\n", changed)

    def test_adds_missing_key_to_existing_section(self):
        self.assertEqual(set_ini_key("[ScaleNG]\nfoo=1\n[bridge]\n", "ScaleNG", "realInputs", "0"),
                         "[ScaleNG]\nfoo=1\nrealInputs=0\n[bridge]\n")


if __name__ == "__main__":
    unittest.main()
