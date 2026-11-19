"""The result type every check returns."""

from dataclasses import dataclass


@dataclass(frozen=True)
class CheckResult:
    """One named check: whether it passed, and a human-readable detail line."""

    name: str
    ok: bool
    detail: str
