#!/usr/bin/env python3
"""Stdlib-only regression tests for the actual service runner's LSan parser."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("selected_service_runner", ROOT / "tools/run-selected-service-boundary.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
EXPECTED = json.loads((ROOT / "tests/selected-services/legacy-opendir.expected.json").read_text())
RULE = "^skyline::vfs::OsFileSystem::OpenDirectoryImpl("
TABLE = "-----------------------------------------------------\nSuppressions used:\n  count      bytes template\n      6     196896 " + RULE + "\n-----------------------------------------------------\n"


class LsanSummaryTests(unittest.TestCase):
    def reject(self, text):
        with self.assertRaises(ValueError):
            RUNNER.parse_lsan_summary(text, EXPECTED)

    def test_exact_known_profile(self):
        self.assertEqual(RUNNER.parse_lsan_summary(TABLE, EXPECTED),
                         {"allocations": 6, "bytes": 196896, "allocation_bytes": 32816})

    def test_absent_summary(self):
        self.reject("")

    def test_matching_row_without_table(self):
        self.reject("6 196896 " + RULE + "\n")

    def test_wrong_six_times_40000(self):
        self.reject(TABLE.replace("196896", "240000"))

    def test_wrong_count(self):
        self.reject(TABLE.replace("      6", "      5"))

    def test_duplicate_tables(self):
        self.reject(TABLE + TABLE)

    def test_duplicate_rule(self):
        self.reject(TABLE.replace("      6     196896 " + RULE,
                                  ("      6     196896 " + RULE + "\n") * 2))

    def test_other_suppression(self):
        self.reject(TABLE.replace("      6     196896 " + RULE,
                                  "      6     196896 " + RULE + "\n      1     123 other_function"))

    def test_matching_row_outside_table(self):
        self.reject("6 196896 " + RULE + "\n" + TABLE)

    def test_wrong_function(self):
        self.reject(TABLE.replace(RULE, "^other::OpenDirectoryImpl("))

    def test_confusing_header(self):
        self.reject(TABLE.replace("count      bytes template", "count      objects template"))

    def test_unterminated_table(self):
        self.reject(TABLE.rsplit("-----------------------------------------------------", 1)[0])

    def test_inconsistent_expected_profile(self):
        with self.assertRaises(ValueError):
            RUNNER.parse_lsan_summary(TABLE, {**EXPECTED, "expected_allocation_bytes": 40000})


if __name__ == "__main__":
    unittest.main(verbosity=2)
