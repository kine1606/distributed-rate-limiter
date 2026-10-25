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


def reset_after_ms(now_ms: int, monthly: bool) -> int:
    """Milliseconds until the current period ends."""
    day = now_ms // DAY_MS
    if not monthly:
        return (day + 1) * DAY_MS - now_ms
    date = EPOCH + dt.timedelta(days=day)
    first_next = dt.date(date.year + date.month // 12, date.month % 12 + 1, 1)
    return (first_next - EPOCH).days * DAY_MS - now_ms
