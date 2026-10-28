"""Merchant webhooks: where alerts go, how they are signed, and which addresses the platform refuses to
call. The platform sends requests to URLs its merchants choose, so a URL that resolves to a private,
loopback or link-local address (another service on the platform's network, a cloud metadata endpoint)
is refused when it is registered and again before every delivery. RL_ALLOW_PRIVATE_WEBHOOKS=1 lifts
that, for a demo on one machine."""
import hashlib
import hmac
import ipaddress
import os
import socket
import time
import urllib.parse


class UnsafeUrl(ValueError):
    pass


def private_allowed():
    return os.environ.get("RL_ALLOW_PRIVATE_WEBHOOKS") == "1"


def check_url(url, allow_private=None):
    """Raises UnsafeUrl unless `url` is http(s) to a public address (or private ones are allowed).
    Returns the address it checked (None when private ones are allowed): a delivery must connect to
    it, not resolve the name again, or the name's DNS could answer a private address the second time
    (DNS rebinding)."""
    if allow_private is None:
        allow_private = private_allowed()
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        raise UnsafeUrl("the URL must be http or https with a host")
    if parts.username or parts.password:
        raise UnsafeUrl("the URL must not carry credentials")
    if allow_private:
        return None
    try:
        infos = socket.getaddrinfo(parts.hostname, parts.port or (443 if parts.scheme == "https" else 80))
    except socket.gaierror as e:
        raise UnsafeUrl(f"cannot resolve {parts.hostname}") from e
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        if ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_multicast or ip.is_reserved or ip.is_unspecified:
            raise UnsafeUrl(f"{parts.hostname} resolves to a non-public address")
    return infos[0][4][0]


def pinned(url, address):
    """The request for `url` sent to `address`: the URL with that address as its host, the Host header,
    and for https the name the certificate is checked against (httpx's `sni_hostname`)."""
    parts = urllib.parse.urlsplit(url)
    if address is None:
        return url, {}, {}
    host = f"[{address}]" if ":" in address else address
    netloc = host + (f":{parts.port}" if parts.port else "")
    extensions = {"sni_hostname": parts.hostname} if parts.scheme == "https" else {}
    return parts._replace(netloc=netloc).geturl(), {"Host": parts.netloc}, extensions


def sign(secret, body, at=None):
    """The X-RL-Signature header: t=<unix seconds>,v1=<HMAC-SHA256 of "<t>.<body>" with the secret>."""
    at = int(time.time()) if at is None else at
    mac = hmac.new(secret.encode(), f"{at}.".encode() + body, hashlib.sha256).hexdigest()
    return f"t={at},v1={mac}"


def verify(secret, header, body, tolerance_s=300, now=None):
    """What a receiver does: the signature matches and is recent (a replayed delivery is refused)."""
    try:
        parts = dict(p.split("=", 1) for p in header.split(","))
        at = int(parts["t"])
    except (ValueError, KeyError):
        return False
    now = int(time.time()) if now is None else now
    expected = sign(secret, body, at).split("v1=", 1)[1]
    return abs(now - at) <= tolerance_s and hmac.compare_digest(expected, parts.get("v1", ""))
