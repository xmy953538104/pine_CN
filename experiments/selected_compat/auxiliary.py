# SPDX-License-Identifier: MPL-2.0
"""Offline, non-admitting structural experiment over synthetic member bytes.

This recognizes only a deliberately narrow layout compatible with Pine's
fixed-size Common Ticket reader at b25e9e41. It authenticates nothing: arbitrary
opaque-bit corruption and a deliberately constructed mimic are indistinguishable
from an untrusted layout. There is no credential, filesystem, or Loader API here.
"""

from dataclasses import dataclass
from enum import Enum
import json
from typing import Protocol


HARD_MAX_MEMBER_BYTES = 0x3C0
HARD_MAX_MEMBERS = 32
HARD_MAX_TOTAL_BYTES = 32 * 1024
HARD_MAX_READ_CALLS = 32
MAX_REPORT_BYTES = 1024


class Hint(str, Enum):
    NONE = "none"
    TICKET = "ticket"
    CERTIFICATE = "certificate"
    NCA = "nca"
    OTHER = "other"


class Classification(str, Enum):
    TICKET = "recognized_untrusted_ticket_structure"
    UNKNOWN = "unknown"
    REJECTED = "rejected"
    UNSUPPORTED = "unsupported"


class Code(str, Enum):
    TICKET_STRUCTURE = "ticket_structure"
    UNKNOWN_STRUCTURE = "unknown_structure"
    CERTIFICATE_UNSUPPORTED = "certificate_profile_not_supported"
    HINT_CONFLICT = "hint_conflict"
    NCA_VERIFIER_REQUIRED = "nca_verifier_required"
    PERSONAL_UNSUPPORTED = "personal_ticket_not_supported"
    INVALID_TITLE_KEY_TYPE = "invalid_title_key_type"
    ISSUER_PROFILE_INVALID = "issuer_profile_invalid"
    FIXED_EXTENT_MISMATCH = "fixed_extent_mismatch"
    MEMBER_SIZE_LIMIT = "member_size_limit"
    INVALID_SIZE = "invalid_size"
    SHORT_READ = "short_read"
    INVALID_READ = "invalid_read"
    INPUT_CHANGED = "input_changed"
    READER_FAILURE = "reader_failure"
    INVALID_HINT = "invalid_hint"
    INVALID_BUDGET = "invalid_budget"
    MEMBER_COUNT_LIMIT = "member_count_limit"
    AGGREGATE_BYTE_LIMIT = "aggregate_byte_limit"
    READ_COUNT_LIMIT = "read_count_limit"


class Profile(str, Enum):
    NONE = "none"
    TICKET = "pine_b25_common_ticket_fixed_extent"


class Routing(str, Enum):
    NONE = "none"
    NCA = "nca_verifier_required"


class Reader(Protocol):
    """The caller supplies an already bounded synthetic source, never a path."""

    def size(self) -> int:
        ...

    def read(self, offset: int, count: int) -> bytes:
        ...


class ConfigurationError(ValueError):
    """A fixed code only; input values and exception bodies are never included."""


def _bounded_integer(value: object, maximum: int) -> bool:
    # bool is deliberately excluded even though it subclasses int.
    return type(value) is int and 0 <= value <= maximum


@dataclass(frozen=True)
class Limits:
    """Callers may lower limits; raising a hard cap is never supported."""

    max_member_bytes: int = HARD_MAX_MEMBER_BYTES
    max_members: int = HARD_MAX_MEMBERS
    max_total_bytes: int = HARD_MAX_TOTAL_BYTES
    max_read_calls: int = HARD_MAX_READ_CALLS

    def __post_init__(self) -> None:
        for value, maximum in (
            (self.max_member_bytes, HARD_MAX_MEMBER_BYTES),
            (self.max_members, HARD_MAX_MEMBERS),
            (self.max_total_bytes, HARD_MAX_TOTAL_BYTES),
            (self.max_read_calls, HARD_MAX_READ_CALLS),
        ):
            if not _bounded_integer(value, maximum):
                raise ConfigurationError("invalid_limits")


class Budget:
    """One shared budget per experiment batch; no reset or refund operation.

    Every member attempt is charged before size(), and every read attempt plus
    its full requested extent is charged before read(), including failures.
    Each charged member uses at most two size callbacks and one read callback.
    Exceeding a global budget is sticky and prevents further reader callbacks.
    This bounds classifier requests, not execution time inside a supplied reader.
    """

    __slots__ = ("__limits", "__members", "__reserved", "__reads", "__halt")

    def __init__(self, limits: Limits = Limits()) -> None:
        if type(limits) is not Limits:
            raise ConfigurationError("invalid_limits")
        # Revalidate even a caller-supplied dataclass instance before copying it.
        self.__limits = Limits(limits.max_member_bytes, limits.max_members,
                               limits.max_total_bytes, limits.max_read_calls)
        self.__members = 0
        self.__reserved = 0
        self.__reads = 0
        self.__halt = None

    @property
    def limits(self) -> Limits:
        # Expose a value copy, never the budget's configuration object.
        return Limits(self.__limits.max_member_bytes, self.__limits.max_members,
                      self.__limits.max_total_bytes, self.__limits.max_read_calls)

    def _begin(self) -> tuple[int | None, Code | None]:
        if self.__halt is not None:
            return None, self.__halt
        if self.__members == self.__limits.max_members:
            self.__halt = Code.MEMBER_COUNT_LIMIT
            return None, self.__halt
        ordinal = self.__members
        self.__members += 1
        return ordinal, None

    def _reserve_read(self, count: int) -> Code | None:
        if self.__halt is not None:
            return self.__halt
        if not _bounded_integer(count, self.__limits.max_member_bytes):
            raise ConfigurationError("invalid_budget_charge")
        if self.__reads == self.__limits.max_read_calls:
            self.__halt = Code.READ_COUNT_LIMIT
        elif count > self.__limits.max_total_bytes - self.__reserved:
            self.__halt = Code.AGGREGATE_BYTE_LIMIT
        else:
            self.__reads += 1
            self.__reserved += count
        return self.__halt

    def snapshot(self) -> dict:
        return {
            "member_attempts": self.__members,
            "reserved_bytes": self.__reserved,
            "read_attempts": self.__reads,
            "halt_code": None if self.__halt is None else self.__halt.value,
        }


@dataclass(frozen=True)
class Result:
    classification: Classification
    code: Code
    hint: Hint
    profile: Profile = Profile.NONE
    routing: Routing = Routing.NONE
    member_ordinal: int | None = None
    declared_size: int | None = None
    read_bytes: int | None = None
    read_complete: bool = False

    def __post_init__(self) -> None:
        for value, kind in ((self.classification, Classification), (self.code, Code),
                            (self.hint, Hint), (self.profile, Profile), (self.routing, Routing)):
            if type(value) is not kind:
                raise ConfigurationError("invalid_result")
        for value, maximum in ((self.member_ordinal, HARD_MAX_MEMBERS - 1),
                               (self.declared_size, HARD_MAX_MEMBER_BYTES),
                               (self.read_bytes, HARD_MAX_MEMBER_BYTES)):
            if value is not None and not _bounded_integer(value, maximum):
                raise ConfigurationError("invalid_result")
        if type(self.read_complete) is not bool:
            raise ConfigurationError("invalid_result")
        if self.read_complete and (self.declared_size is None or self.read_bytes != self.declared_size):
            raise ConfigurationError("invalid_result")

    def to_safe_dict(self) -> dict:
        # Neither field is caller-configurable, even on unsuccessful results.
        return {
            "schema_version": 1,
            "classification": self.classification.value,
            "code": self.code.value,
            "hint": self.hint.value,
            "profile": self.profile.value,
            "routing": self.routing.value,
            "authentication": "not_verified",
            "runtime_admissible": False,
            "member_ordinal": self.member_ordinal,
            "declared_size": self.declared_size,
            "read_bytes": self.read_bytes,
            "read_complete": self.read_complete,
        }

    def to_json(self) -> str:
        text = json.dumps(self.to_safe_dict(), sort_keys=True, separators=(",", ":"))
        if len(text.encode("ascii")) > MAX_REPORT_BYTES:
            raise ConfigurationError("report_size_limit")
        return text


# The six little-endian signature declarations and body offsets are exactly the
# switch in Pine b25e9e41 vfs/ticket.cpp. Only fixed shape is used, never crypto.
_BODY_OFFSETS = {
    0x010000: 0x240,
    0x010001: 0x140,
    0x010002: 0x80,
    0x010003: 0x240,
    0x010004: 0x140,
    0x010005: 0x80,
}
_BODY_BYTES = 0x180
_NCA_MARKERS = (b"NCA0", b"NCA1", b"NCA2", b"NCA3")


def classify(reader: Reader, budget: Budget, hint: Hint = Hint.NONE) -> Result:
    """Classify one member, without admitting it or retaining any member bytes.

    Known NCA marker sentinels are routed away; this is not an NCA verifier or
    encrypted-header detector. Certificate structure is explicitly unsupported.
    Only enums may describe hints; filenames, paths, and credentials are absent
    from the API. Exceptions from callbacks are converted to a fixed code and
    never retried. Callers must retain the same Budget for the whole batch.
    """
    if type(budget) is not Budget:
        return Result(Classification.REJECTED, Code.INVALID_BUDGET,
                      hint if type(hint) is Hint else Hint.NONE)
    ordinal, failure = budget._begin()
    safe_hint = hint if type(hint) is Hint else Hint.NONE
    size = None
    read_bytes = None
    complete = False

    def result(kind: Classification, code: Code, *, profile: Profile = Profile.NONE,
               routing: Routing = Routing.NONE) -> Result:
        return Result(kind, code, safe_hint, profile, routing, ordinal, size, read_bytes, complete)

    if failure is not None:
        return result(Classification.REJECTED, failure)
    if type(hint) is not Hint:
        return result(Classification.REJECTED, Code.INVALID_HINT)
    # The hint is only a rejection/routing input; it never authorizes a member.
    if hint is Hint.NCA:
        return result(Classification.REJECTED, Code.NCA_VERIFIER_REQUIRED, routing=Routing.NCA)
    try:
        declared = reader.size()
    except Exception:
        return result(Classification.REJECTED, Code.READER_FAILURE)
    if type(declared) is not int or declared < 0:
        return result(Classification.REJECTED, Code.INVALID_SIZE)
    if declared > budget.limits.max_member_bytes:
        return result(Classification.REJECTED, Code.MEMBER_SIZE_LIMIT)
    size = declared
    failure = budget._reserve_read(size)
    if failure is not None:
        return result(Classification.REJECTED, failure)
    try:
        data = reader.read(0, size)
    except Exception:
        return result(Classification.REJECTED, Code.READER_FAILURE)
    if type(data) is not bytes or len(data) > size:
        return result(Classification.REJECTED, Code.INVALID_READ)
    read_bytes = len(data)
    if read_bytes != size:
        return result(Classification.REJECTED, Code.SHORT_READ)
    try:
        after = reader.size()
    except Exception:
        return result(Classification.REJECTED, Code.READER_FAILURE)
    if type(after) is not int or after < 0:
        return result(Classification.REJECTED, Code.INVALID_SIZE)
    if after != size:
        return result(Classification.REJECTED, Code.INPUT_CHANGED)
    complete = True
    if data[:4] in _NCA_MARKERS or data[0x200:0x204] in _NCA_MARKERS:
        return result(Classification.REJECTED, Code.NCA_VERIFIER_REQUIRED, routing=Routing.NCA)
    body = _BODY_OFFSETS.get(int.from_bytes(data[:4], "little")) if len(data) >= 4 else None
    if body is None:
        if hint is Hint.CERTIFICATE:
            return result(Classification.UNSUPPORTED, Code.CERTIFICATE_UNSUPPORTED)
        return result(Classification.UNKNOWN, Code.UNKNOWN_STRUCTURE)
    if hint not in (Hint.NONE, Hint.TICKET):
        return result(Classification.REJECTED, Code.HINT_CONFLICT)
    if size != body + _BODY_BYTES:
        return result(Classification.REJECTED, Code.FIXED_EXTENT_MISMATCH)
    title_key_type = data[body + 0x141]
    if title_key_type == 1:
        return result(Classification.UNSUPPORTED, Code.PERSONAL_UNSUPPORTED)
    if title_key_type != 0:
        return result(Classification.REJECTED, Code.INVALID_TITLE_KEY_TYPE)
    issuer = data[body:body + 0x40]
    end = issuer.find(b"\0")
    # This is an additional local synthetic-profile restriction, not a claim
    # about Pine's validation or a complete normative Ticket issuer grammar.
    if end <= 0 or any(c < 0x20 or c > 0x7E for c in issuer[:end]) or any(issuer[end:]):
        return result(Classification.REJECTED, Code.ISSUER_PROFILE_INVALID)
    return result(Classification.TICKET, Code.TICKET_STRUCTURE, profile=Profile.TICKET)
