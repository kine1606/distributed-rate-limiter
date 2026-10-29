"""Quota periods named as lua/quota.lua names them: the UTC day "d<days since 1970-01-01>", or the month
"m<year * 12 + month - 1>". A month starts on its anchor day (P5: billing from the subscription date), 1 to
28, and is numbered by the calendar month it starts in; anchor 1, the default, is the calendar month. A
top-up names its period this way, so the two must agree (control/tests/test_periods.py checks them against
the script)."""
import datetime as dt

DAY_MS = 86_400_000
EPOCH = dt.date(1970, 1, 1)


def anchor_of(plan: dict) -> int:
    """A plan hash's anchor day, as src/plan/plan.h reads it: 1 to 28, anything else 1."""
    value = plan.get("anchor", "1")
    return int(value) if value.isdigit() and 1 <= int(value) <= 28 else 1


def anchor_at(now_ms: int) -> int:
    """The anchor of a subscription that starts at `now_ms`: its day of the month, the 29th to 31st billed
    on the 28th, so every month has the day."""
    return min(28, (EPOCH + dt.timedelta(days=now_ms // DAY_MS)).day)


def period_name(now_ms: int, monthly: bool, anchor: int = 1) -> str:
    return f"m{month_of(now_ms, anchor)}" if monthly else f"d{now_ms // DAY_MS}"


def month_of(now_ms: int, anchor: int = 1) -> int:
    """The month `now_ms` falls in, as lua/quota.lua numbers it: the billing month."""
    date = EPOCH + dt.timedelta(days=now_ms // DAY_MS)
    month = date.year * 12 + date.month - 1
    return month - 1 if date.day < anchor else month


def month_id(label: str) -> int:
    """"2026-11" -> 2026 * 12 + 10. ValueError if it is not a month."""
    year, sep, month = label.partition("-")
    if not (sep and len(year) == 4 and len(month) == 2 and year.isdigit() and month.isdigit() and 1 <= int(month) <= 12):
        raise ValueError(f"not a month (YYYY-MM): {label!r}")
    return int(year) * 12 + int(month) - 1


def month_label(month: int) -> str:
    return f"{month // 12:04d}-{month % 12 + 1:02d}"


def month_bounds_ms(month: int, anchor: int = 1) -> tuple[int, int]:
    """[start, end) of a month in Unix ms, UTC: from its anchor day to the same day of the next month."""
    def start_ms(m):
        return (dt.date(m // 12, m % 12 + 1, anchor) - EPOCH).days * DAY_MS
    return start_ms(month), start_ms(month + 1)


def reset_after_ms(now_ms: int, monthly: bool, anchor: int = 1) -> int:
    """Milliseconds until the current period ends."""
    if not monthly:
        return (now_ms // DAY_MS + 1) * DAY_MS - now_ms
    return month_bounds_ms(month_of(now_ms, anchor), anchor)[1] - now_ms
