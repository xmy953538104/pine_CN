#!/usr/bin/env python3
"""Synthetic clock/state tests only. No real counters, threads, clocks or files."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from present_watchdog.model import (Config, Evidence, Field, Observation, Packet,
                                    Phase, SECOND, U64_MAX, Watchdog)


def observed(second, count, session=1, **changes):
    values = dict(session=session, observed_ns=second * SECOND, present_returns=count,
                  normal_progress_confirmed=True, paused=False, foreground=True, surface_available=True)
    values.update(changes)
    return Observation(**values)


def start(config=None):
    model = Watchdog(config or Config(enabled=True))
    assert model.start_session(1, 0)
    return model


def arm(model):
    for second in range(3):
        result = model.tick(second * SECOND, observed(second, second))
        assert not result.events
    assert result.phase == Phase.WATCHING


def warn(model, packet=None):
    arm(model)
    for second in range(3, 7):
        result = model.tick(second * SECOND, observed(second, 2), packet)
    assert result.reason == "threshold" and len(result.events) == 1
    return result.events[0]


class WatchdogModelTests(unittest.TestCase):
    def test_default_off_does_not_even_inspect_inputs(self):
        model = Watchdog()
        self.assertFalse(model.start_session(object(), object()))
        self.assertEqual(model.tick(object(), object(), object()).reason, "off")
        self.assertFalse(model.end_session(object()))
        self.assertIsNone(model.session)
        self.assertEqual(model.phase, Phase.DISABLED)

    def test_never_started_or_unknown_phase_does_not_arm(self):
        for ready in (False, None):
            model = start()
            for second in range(100):
                result = model.tick(second * SECOND, observed(second, second, normal_progress_confirmed=ready))
                self.assertEqual(result.phase, Phase.SUSPENDED)
                self.assertFalse(result.events)

    def test_first_present_and_one_advance_are_insufficient(self):
        model = start()
        model.tick(0, observed(0, 1000))
        model.tick(SECOND, observed(1, 1001))
        for second in range(2, 40):
            result = model.tick(second * SECOND, observed(second, 1001))
            self.assertEqual(result.phase, Phase.WAITING)
            self.assertFalse(result.events)

    def test_ordinary_progress_never_warns(self):
        model = start()
        for second in range(120):
            result = model.tick(second * SECOND, observed(second, second * 3))
            self.assertFalse(result.events)
        self.assertEqual(result.phase, Phase.WATCHING)
        self.assertEqual(model._advances, 2)

    def test_arming_increases_must_be_consecutive_eligible_polls(self):
        model = start()
        model.tick(0, observed(0, 0))
        model.tick(SECOND, observed(1, 1))
        model.tick(2 * SECOND, observed(2, 1))
        self.assertEqual(model.tick(3 * SECOND, observed(3, 2)).phase, Phase.WAITING)
        self.assertEqual(model.tick(4 * SECOND, observed(4, 3)).phase, Phase.WATCHING)

    def test_four_second_boundary_and_episode_deduplication(self):
        model = start()
        arm(model)
        for second in (3, 4, 5):
            self.assertFalse(model.tick(second * SECOND, observed(second, 2)).events)
        event = model.tick(6 * SECOND, observed(6, 2)).events[0]
        self.assertEqual(event.payload["kind"], "no_new_present_observed")
        self.assertEqual(event.payload["no_progress_observation_ns"], 4 * SECOND)
        self.assertEqual(event.payload["cause"], "undetermined")
        self.assertEqual(event.payload["data_kind"], "synthetic_offline")
        for second in range(7, 70):
            self.assertEqual(model.tick(second * SECOND, observed(second, 2)).reason, "episode_already_reported")

    def test_recovery_is_counter_observation_not_game_recovery(self):
        model = start()
        warn(model)
        result = model.tick(7 * SECOND, observed(7, 3))
        self.assertEqual(result.events[0].payload["kind"], "present_observation_resumed")
        self.assertEqual(result.phase, Phase.WAITING)
        self.assertEqual(model.tick(8 * SECOND, observed(8, 4)).phase, Phase.WATCHING)

    def test_cooldown_defers_a_distinct_episode_and_does_not_repeat(self):
        model = start()
        warn(model)
        model.tick(7 * SECOND, observed(7, 3))
        model.tick(8 * SECOND, observed(8, 4))
        for second in range(9, 36):
            result = model.tick(second * SECOND, observed(second, 4))
            self.assertFalse(result.events)
        self.assertEqual(result.reason, "cooldown")
        result = model.tick(36 * SECOND, observed(36, 4))
        self.assertEqual(result.events[0].payload["episode"], 2)
        self.assertFalse(model.tick(37 * SECOND, observed(37, 4)).events)

    def test_per_session_warning_budget_also_bounds_recoveries(self):
        model = start(Config(enabled=True, max_alerts_per_session=1))
        warn(model)
        recovery = model.tick(7 * SECOND, observed(7, 3))
        self.assertEqual(len(recovery.events), 1)
        model.tick(8 * SECOND, observed(8, 4))
        for second in range(9, 70):
            result = model.tick(second * SECOND, observed(second, 4))
            self.assertFalse(result.events)
        self.assertEqual(result.reason, "session_alert_budget")
        self.assertFalse(model.tick(70 * SECOND, observed(70, 5)).events)

    def test_pause_background_missing_surface_and_unknown_flags_reset(self):
        for change in ({"paused": True}, {"foreground": False}, {"surface_available": False},
                       {"paused": None}, {"foreground": None}, {"surface_available": None}):
            model = start()
            arm(model)
            for second in (3, 4, 5):
                model.tick(second * SECOND, observed(second, 2))
            result = model.tick(6 * SECOND, observed(6, 2, **change))
            self.assertEqual(result.phase, Phase.SUSPENDED)
            for second in range(7, 15):
                self.assertFalse(model.tick(second * SECOND, observed(second, 2)).events)
            self.assertEqual(model.phase, Phase.WAITING)

    def test_lifecycle_veto_precedes_rate_limiting(self):
        model = start()
        arm(model)
        value = observed(2, 2, paused=True)
        self.assertEqual(model.tick(2 * SECOND + 1, value).phase, Phase.SUSPENDED)

    def test_exit_at_threshold_and_late_callbacks_cannot_warn(self):
        model = start()
        arm(model)
        for second in (3, 4, 5):
            model.tick(second * SECOND, observed(second, 2))
        result = model.tick(6 * SECOND, observed(6, 2, exit_requested=True))
        self.assertEqual(result.phase, Phase.EXITED)
        self.assertFalse(result.events)
        self.assertFalse(model.tick(100 * SECOND, observed(100, 2)).events)
        self.assertFalse(model.start_session(1, 100 * SECOND))

    def test_explicit_end_is_terminal_for_the_active_generation(self):
        model = start()
        arm(model)
        self.assertFalse(model.end_session(0))
        self.assertTrue(model.end_session(1))
        self.assertEqual(model.tick(6 * SECOND, observed(6, 2)).phase, Phase.EXITED)

    def test_session_switch_drops_old_counter_and_snapshot_state(self):
        model = start()
        warn(model)
        self.assertTrue(model.start_session(2, 7 * SECOND))
        old = model.tick(100 * SECOND, observed(100, U64_MAX, session=1))
        self.assertEqual(old.reason, "foreign_session_ignored")
        self.assertFalse(model.start_session(1, 100 * SECOND))
        for second in range(7, 20):
            result = model.tick(second * SECOND, observed(second, 2, session=2))
            self.assertFalse(result.events)
        self.assertEqual(result.phase, Phase.WAITING)

    def test_clock_regression_discards_elapsed_progress(self):
        model = start()
        arm(model)
        result = model.tick(SECOND, observed(1, 2))
        self.assertEqual(result.reason, "clock_regressed")
        for second in range(3, 10):
            self.assertFalse(model.tick(second * SECOND, observed(second, 2)).events)
        self.assertEqual(model.phase, Phase.WAITING)

    def test_counter_regression_requires_two_new_advances(self):
        model = start()
        arm(model)
        self.assertEqual(model.tick(3 * SECOND, observed(3, 0)).reason, "counter_reset")
        for second in range(4, 10):
            self.assertFalse(model.tick(second * SECOND, observed(second, 0)).events)
        self.assertEqual(model.tick(10 * SECOND, observed(10, 1)).phase, Phase.WAITING)
        self.assertEqual(model.tick(11 * SECOND, observed(11, 2)).phase, Phase.WATCHING)

    def test_observer_gap_is_not_assumed_counter_silence(self):
        model = start()
        arm(model)
        result = model.tick(100 * SECOND, observed(100, 2))
        self.assertEqual(result.reason, "observer_gap")
        self.assertFalse(result.events)
        for second in range(101, 110):
            self.assertFalse(model.tick(second * SECOND, observed(second, 2)).events)

    def test_counter_failure_stale_future_or_repeated_sample_disarms(self):
        variants = [None, observed(6, None), observed(0, 2), observed(7, 2), observed(2, 2)]
        for value in variants:
            model = start()
            arm(model)
            self.assertFalse(model.tick(6 * SECOND, value).events)
            self.assertEqual(model.phase, Phase.WAITING)
        model = start()
        arm(model)
        result = model.tick(3 * SECOND, observed(2, 2))
        self.assertEqual(result.reason, "sample_not_advancing")

    def test_faster_ticks_do_not_advance_detection_or_sample_qualification(self):
        model = start()
        model.tick(0, observed(0, 0))
        for ns in (1, 100, SECOND - 1):
            self.assertEqual(model.tick(ns, Observation(1, ns, 10, True, False, True, True)).reason, "rate_limited")
        self.assertEqual(model.tick(SECOND, observed(1, 1)).phase, Phase.WAITING)

    def test_optional_snapshot_unknown_stale_and_unsupported_are_distinct(self):
        packet = Packet(1, 100, (
            Evidence(Field.GUEST_THREAD, "observed", 6 * SECOND, "waiting"),
            Evidence(Field.CPU_PROGRESS, "observed", 0, 99),
            Evidence(Field.GPU_FENCE, "unsupported"),
        ))
        event = warn(start(), packet)
        records = {item["field"]: item for item in event.payload["snapshot"]["fields"]}
        self.assertEqual(records["guest_thread_state"]["source"], "future_safe_point")
        self.assertEqual(records["guest_thread_state"]["value"], "waiting")
        self.assertEqual(records["cpu_progress"]["status"], "stale")
        self.assertNotIn("value", records["cpu_progress"])
        self.assertEqual(records["gpu_fence_state"]["status"], "unsupported")
        self.assertEqual(records["host_thread_state"]["status"], "unavailable")
        self.assertFalse(event.payload["snapshot"]["collector_deadline_measured"])

    def test_missing_or_foreign_snapshot_does_not_hide_primary_counter_evidence(self):
        for packet, status in ((None, "not_connected"), (Packet(2, 0), "session_mismatch")):
            event = warn(start(), packet)
            self.assertEqual(event.payload["kind"], "no_new_present_observed")
            self.assertEqual(event.payload["snapshot"]["status"], status)

    def test_declared_time_field_and_byte_budgets_drop_optional_payload(self):
        for packet in (Packet(1, 5_000_001), Packet(1, 0, tuple(Evidence(Field.CPU_PROGRESS) for _ in range(12)))):
            event = warn(start(), packet)
            self.assertEqual(event.payload["snapshot"]["status"], "budget_exceeded")
            self.assertLessEqual(len(event.to_json().encode("ascii")), 4096)
        event = warn(start(Config(enabled=True, max_event_bytes=1024)))
        self.assertEqual(event.payload["snapshot"]["status"], "byte_budget_exceeded")
        self.assertLessEqual(len(event.to_json().encode("ascii")), 1024)

    def test_duplicate_evidence_is_not_silently_selected(self):
        packet = Packet(1, 0, (Evidence(Field.CPU_PROGRESS), Evidence(Field.CPU_PROGRESS)))
        self.assertEqual(warn(start(), packet).payload["snapshot"]["status"], "invalid_duplicate_fields")

    def test_large_packets_and_freeform_private_strings_are_rejected(self):
        with self.assertRaises(ValueError):
            Packet(1, 0, (Evidence(Field.CPU_PROGRESS),) * 33)
        with self.assertRaises(ValueError):
            Packet(1, 0, iter(()))
        with self.assertRaises(ValueError):
            Evidence(Field.EXCEPTION, "observed", 0, "PRIVATE path and stack")
        with self.assertRaises(ValueError):
            Evidence(Field.GUEST_THREAD, "unavailable", None, "running")
        with self.assertRaises(ValueError):
            observed(0, True)

    def test_threshold_configuration_range_and_known_snapshot_origins(self):
        for threshold in (3, 4, 5):
            self.assertEqual(Config(threshold_ns=threshold * SECOND).threshold_ns, threshold * SECOND)
        for bad in (2, 6):
            with self.assertRaises(ValueError):
                Config(threshold_ns=bad * SECOND)
        packet = Packet(1, 0, (Evidence(Field.ACCEPTED, "observed", 6 * SECOND, 2),
                               Evidence(Field.LAST_RESULT, "observed", 6 * SECOND, -4)))
        payload = json.loads(warn(start(), packet).to_json())
        record = next(value for value in payload["snapshot"]["fields"] if value["field"] == "present_accepted")
        self.assertEqual(record["source"], "existing_counter_future_snapshot_adapter")
        self.assertEqual(payload["visibility"], "not_observed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
