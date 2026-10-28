"""Quota periods named as lua/quota.lua names them: the UTC day "d<days since 1970-01-01>", or the UTC
calendar month "m<year * 12 + month - 1>". A top-up names its period this way, so the two must agree
(control/tests/test_periods.py checks them against the script)."""
import datetime as dt

DAY_MS = 86_400_000
EPOCH = dt.date(1970, 1, 1)


def period_name(now_ms: int, monthly: bool) -> str:
    day = now_ms // DAY_MS
    if not monthly:
        return f"d{day}"
    date = EPOCH + dt.timedelta(days=day)
    return f"m{date.year * 12 + date.month - 1}"


def month_of(now_ms: int) -> int:
    """The calendar month as lua/quota.lua numbers it (year * 12 + month - 1): the billing month."""
    return int(period_name(now_ms, True)[1:])


def month_id(label: str) -> int:
    """"2026-11" -> 2026 * 12 + 10. ValueError if it is not a month."""
    year, sep, month = label.partition("-")
    if not (sep and len(year) == 4 and len(month) == 2 and year.isdigit() and month.isdigit() and 1 <= int(month) <= 12):
        raise ValueError(f"not a month (YYYY-MM): {label!r}")
    return int(year) * 12 + int(month) - 1


def month_label(month: int) -> str:
    return f"{month // 12:04d}-{month % 12 + 1:02d}"


def month_bounds_ms(month: int) -> tuple[int, int]:
    """[start, end) of a calendar month in Unix ms, UTC."""
    def first_day_ms(m):
        return (dt.date(m // 12, m % 12 + 1, 1) - EPOCH).days * DAY_MS
    return first_day_ms(month), first_day_ms(month + 1)


def reset_after_ms(now_ms: int, monthly: bool) -> int:
    """Milliseconds until the current period ends."""
    day = now_ms // DAY_MS
    if not monthly:
        return (day + 1) * DAY_MS - now_ms
    date = EPOCH + dt.timedelta(days=day)
    first_next = dt.date(date.year + date.month // 12, date.month % 12 + 1, 1)
    return (first_next - EPOCH).days * DAY_MS - now_ms
