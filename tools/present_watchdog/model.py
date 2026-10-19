#!/usr/bin/env python3
"""Default-OFF offline state machine. All clocks/counters are supplied synthetic data.

No thread, timer, syscall, JNI, signal handler, file input or collection callback.
The optional packet budget is a model contract, not a measured collector deadline.
"""
from dataclasses import dataclass
from enum import Enum
import json

SECOND = 1_000_000_000
U64_MAX = (1 << 64) - 1


def _uint(value):
    if type(value) is not int or not 0 <= value <= U64_MAX:
        raise ValueError("expected bounded unsigned integer")
    return value


class Phase(str, Enum):
    DISABLED = "disabled"
    IDLE = "idle"
    WAITING = "waiting_for_progress"
    WATCHING = "watching"
    STALLED = "no_new_present_observed"
    SUSPENDED = "suspended"
    EXITED = "exited"


class Field(str, Enum):
    ACCEPTED = "present_accepted"
    OTHER = "present_other_results"
    WRAPPER_EXCEPTIONS = "present_wrapper_exceptions"
    LAST_RESULT = "last_present_result"
    LAST_STAGE = "last_success_stage"
    GUEST_THREAD = "guest_thread_state"
    HOST_THREAD = "host_thread_state"
    CPU_PROGRESS = "cpu_progress"
    GPU_SUBMITS = "gpu_submissions"
    GPU_FENCE = "gpu_fence_state"
    EXCEPTION = "recent_exception_code"


THREAD_STATES = {"running", "waiting", "sleeping", "stopped", "unknown"}
VALUES = {
    Field.LAST_STAGE: {"unknown", "os_initialized", "selected_committed", "guest_phase_reported"},
    Field.GUEST_THREAD: THREAD_STATES,
    Field.HOST_THREAD: THREAD_STATES,
    Field.GPU_FENCE: {"signaled", "unsignaled", "unknown"},
    Field.EXCEPTION: {"none", "svc_break", "native_signal", "cpp_exception", "unknown"},
}
ORIGIN = {field: "future_safe_point" for field in Field}
ORIGIN.update({field: "existing_counter_future_snapshot_adapter" for field in
               (Field.ACCEPTED, Field.OTHER, Field.WRAPPER_EXCEPTIONS, Field.LAST_RESULT)})
ORIGIN[Field.LAST_STAGE] = "existing_stage_future_snapshot_adapter"


@dataclass(frozen=True)
class Config:
    enabled: bool = False
    threshold_ns: int = 4 * SECOND
    poll_interval_ns: int = SECOND
    max_sample_age_ns: int = SECOND
    max_observer_gap_ns: int = 2 * SECOND
    cooldown_ns: int = 30 * SECOND
    max_alerts_per_session: int = 3
    snapshot_budget_ns: int = 5_000_000
    max_packet_fields: int = len(Field)
    max_event_bytes: int = 4096

    def __post_init__(self):
        if type(self.enabled) is not bool:
            raise ValueError("invalid enabled flag")
        for name in ("threshold_ns", "poll_interval_ns", "max_sample_age_ns", "max_observer_gap_ns",
                     "cooldown_ns", "max_alerts_per_session", "snapshot_budget_ns", "max_packet_fields", "max_event_bytes"):
            _uint(getattr(self, name))
        if not (3 * SECOND <= self.threshold_ns <= 5 * SECOND and
                SECOND // 2 <= self.poll_interval_ns <= SECOND and
                0 < self.max_sample_age_ns <= SECOND and
                2 * self.poll_interval_ns <= self.max_observer_gap_ns <= 2 * SECOND and
                self.threshold_ns <= self.cooldown_ns <= 60 * SECOND and
                1 <= self.max_alerts_per_session <= 3 and
                0 < self.snapshot_budget_ns <= 5_000_000 and
                1 <= self.max_packet_fields <= len(Field) and 1024 <= self.max_event_bytes <= 4096):
            raise ValueError("invalid bounded model configuration")


@dataclass(frozen=True)
class Observation:
    session: int
    observed_ns: int
    present_returns: int | None
    normal_progress_confirmed: bool | None = None
    paused: bool | None = None
    foreground: bool | None = None
    surface_available: bool | None = None
    exit_requested: bool = False

    def __post_init__(self):
        _uint(self.session)
        _uint(self.observed_ns)
        if self.present_returns is not None:
            _uint(self.present_returns)
        for value in (self.normal_progress_confirmed, self.paused, self.foreground, self.surface_available):
            if value is not None and type(value) is not bool:
                raise ValueError("invalid lifecycle observation")
        if type(self.exit_requested) is not bool:
            raise ValueError("invalid exit observation")


@dataclass(frozen=True)
class Evidence:
    field: Field
    status: str = "unavailable"
    observed_ns: int | None = None
    value: int | str | None = None

    def __post_init__(self):
        if not isinstance(self.field, Field) or self.status not in {"observed", "unavailable", "unsupported"}:
            raise ValueError("invalid fixed evidence field")
        if self.status != "observed":
            if self.value is not None or self.observed_ns is not None:
                raise ValueError("unavailable evidence cannot carry a value")
            return
        _uint(self.observed_ns)
        if self.field in VALUES:
            if type(self.value) is not str or self.value not in VALUES[self.field]:
                raise ValueError("invalid fixed evidence code")
        elif self.field == Field.LAST_RESULT:
            if type(self.value) is not int or not -(1 << 31) <= self.value < (1 << 31):
                raise ValueError("invalid result code")
        else:
            _uint(self.value)


@dataclass(frozen=True)
class Packet:
    session: int
    claimed_cost_ns: int
    fields: tuple[Evidence, ...] = ()

    def __post_init__(self):
        _uint(self.session)
        _uint(self.claimed_cost_ns)
        # Hard admission bound before inspecting entries; no iterable callbacks.
        if type(self.fields) is not tuple or len(self.fields) > 32:
            raise ValueError("packet entry limit")
        if any(not isinstance(value, Evidence) for value in self.fields):
            raise ValueError("invalid packet entry")


@dataclass(frozen=True)
class Event:
    payload: dict

    def to_json(self):
        return json.dumps(self.payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


@dataclass(frozen=True)
class Result:
    phase: Phase
    reason: str
    events: tuple[Event, ...] = ()


class Watchdog:
    def __init__(self, config=Config()):
        self.config = config
        self.phase = Phase.IDLE if config.enabled else Phase.DISABLED
        self.session = None
        self._generation = 0
        self._clock_high = None
        self._last_poll = None
        self._last_sample = None
        self._last_count = None
        self._last_progress = None
        self._advances = 0
        self._episode = 0
        self._reported = False
        self._alerts = 0
        self._last_alert = None

    def _reset_progress(self, phase=Phase.WAITING):
        self.phase = phase
        self._last_sample = self._last_count = self._last_progress = None
        self._advances = 0
        self._reported = False

    def start_session(self, session, now_ns):
        if not self.config.enabled:
            return False
        _uint(session)
        _uint(now_ns)
        # Explicit lifecycle authority only. Old samples cannot start sessions.
        if session <= self._generation:
            return False
        self.session = self._generation = session
        self._clock_high = max(self._clock_high or 0, now_ns)
        self._last_poll = None
        self._alerts = self._episode = 0
        self._last_alert = None
        self._reset_progress()
        return True

    def end_session(self, session):
        if not self.config.enabled or session != self.session:
            return False
        self._reset_progress(Phase.EXITED)
        self.session = None
        return True

    def _snapshot(self, packet, now_ns):
        status = "not_connected"
        supplied = {}
        if packet is not None:
            if packet.session != self.session:
                status = "session_mismatch"
            elif packet.claimed_cost_ns > self.config.snapshot_budget_ns or len(packet.fields) > self.config.max_packet_fields:
                status = "budget_exceeded"
            elif len({item.field for item in packet.fields}) != len(packet.fields):
                status = "invalid_duplicate_fields"
            else:
                status = "supplied_synthetic_packet"
                supplied = {item.field: item for item in packet.fields}
        fields = []
        for name in Field:
            item = supplied.get(name)
            record = {"field": name.value, "source": ORIGIN[name],
                      "status": "unavailable" if status == "supplied_synthetic_packet" else status}
            if item is not None:
                record["status"] = item.status
                if item.status == "observed":
                    age = now_ns - item.observed_ns
                    if age < 0 or age > self.config.max_sample_age_ns:
                        record["status"] = "stale"
                    else:
                        record.update(value=item.value, age_ns=age)
            fields.append(record)
        return {"status": status, "collector_deadline_measured": False, "fields": fields}

    def _event(self, kind, now_ns, observation, packet=None):
        payload = {"schema": 1, "data_kind": "synthetic_offline", "kind": kind,
                   "session_token": self.session, "episode": self._episode, "observed_ns": now_ns,
                   "present_returns": observation.present_returns, "counter_observed_ns": observation.observed_ns,
                   "no_progress_observation_ns": now_ns - self._last_progress,
                   "threshold_ns": self.config.threshold_ns, "cause": "undetermined", "visibility": "not_observed",
                   "snapshot": self._snapshot(packet, now_ns)}
        event = Event(payload)
        if len(event.to_json().encode("ascii")) > self.config.max_event_bytes:
            payload["snapshot"] = {"status": "byte_budget_exceeded", "collector_deadline_measured": False, "fields": []}
            event = Event(payload)
        if len(event.to_json().encode("ascii")) > self.config.max_event_bytes:
            raise AssertionError("fixed event header exceeds configured bound")
        return event

    def tick(self, now_ns, observation=None, packet=None):
        if not self.config.enabled:
            return Result(Phase.DISABLED, "off")
        _uint(now_ns)
        if observation is not None and not isinstance(observation, Observation):
            raise ValueError("invalid model observation")
        if packet is not None and not isinstance(packet, Packet):
            raise ValueError("invalid model packet")
        if self.session is None:
            return Result(self.phase, "no_active_session")
        if observation is not None and observation.session != self.session:
            return Result(self.phase, "foreign_session_ignored")
        if observation is not None and observation.exit_requested:
            self.end_session(self.session)
            return Result(self.phase, "exit_requested")
        if now_ns < self._clock_high:
            self._reset_progress()
            return Result(self.phase, "clock_regressed")
        self._clock_high = now_ns
        # Lifecycle vetoes are immediate even on an otherwise rate-limited tick.
        if observation is not None and (observation.paused is not False or observation.foreground is not True or
                                        observation.surface_available is not True or observation.normal_progress_confirmed is not True):
            self._reset_progress(Phase.SUSPENDED)
            return Result(self.phase, "lifecycle_or_start_unconfirmed")
        if self._last_poll is not None and now_ns - self._last_poll < self.config.poll_interval_ns:
            return Result(self.phase, "rate_limited")
        gap = self._last_poll is not None and now_ns - self._last_poll > self.config.max_observer_gap_ns
        self._last_poll = now_ns
        if observation is None or observation.present_returns is None:
            self._reset_progress()
            return Result(self.phase, "counter_unavailable")
        if not 0 <= now_ns - observation.observed_ns <= self.config.max_sample_age_ns:
            self._reset_progress()
            return Result(self.phase, "sample_stale")
        if self._last_sample is not None and observation.observed_ns <= self._last_sample:
            self._reset_progress()
            return Result(self.phase, "sample_not_advancing")
        if gap:
            self._reset_progress()
        self._last_sample = observation.observed_ns
        count = observation.present_returns
        if self._last_count is None or count < self._last_count:
            reset = self._last_count is not None
            self._last_count, self._last_progress = count, now_ns
            self._advances = 0
            self.phase = Phase.WAITING
            self._reported = False
            return Result(self.phase, "counter_reset" if reset else "observer_gap" if gap else "baseline")
        if count > self._last_count:
            events = ()
            if self.phase == Phase.STALLED:
                if self._reported:
                    events = (self._event("present_observation_resumed", now_ns, observation),)
                self._advances = 0
                self._reported = False
            self._last_count, self._last_progress = count, now_ns
            self._advances = min(2, self._advances + 1)
            self.phase = Phase.WATCHING if self._advances >= 2 else Phase.WAITING
            return Result(self.phase, "progress", events)
        if self.phase == Phase.WAITING:
            self._advances = 0
            return Result(self.phase, "waiting")
        if now_ns - self._last_progress < self.config.threshold_ns:
            return Result(self.phase, "waiting")
        if self.phase != Phase.STALLED:
            self.phase = Phase.STALLED
            self._episode = min(U64_MAX, self._episode + 1)
        if self._reported:
            return Result(self.phase, "episode_already_reported")
        if self._alerts >= self.config.max_alerts_per_session:
            return Result(self.phase, "session_alert_budget")
        if self._last_alert is not None and now_ns - self._last_alert < self.config.cooldown_ns:
            return Result(self.phase, "cooldown")
        self._reported = True
        self._alerts += 1
        self._last_alert = now_ns
        return Result(self.phase, "threshold", (self._event("no_new_present_observed", now_ns, observation, packet),))


def demo():
    """Generated data only; never replay Owner/private diagnostics implicitly."""
    watchdog = Watchdog(Config(enabled=True))
    watchdog.start_session(1, 0)
    for second, count in enumerate((0, 1, 2, 2, 2, 2, 2, 3, 4)):
        observation = Observation(1, second * SECOND, count, True, False, True, True)
        for event in watchdog.tick(second * SECOND, observation).events:
            print(event.to_json())


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", action="store_true", help="print generated offline warning/recovery JSON")
    arguments = parser.parse_args()
    if arguments.demo:
        demo()
    else:
        parser.print_help()
