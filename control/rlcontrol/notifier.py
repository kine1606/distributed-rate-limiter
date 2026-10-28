"""The alert notifier (Phase 2, P2): sends each message lua/quota.lua publishes on rl-alerts to the
webhook of the customer's merchant, signed (webhooks.sign). Off the limiter's request path: the script
only publishes; this process delivers, with retries, on its own threads.

Delivery is at most once while this process is down (Redis Pub/Sub keeps no messages), at least once
while it runs (a retried delivery carries the same X-RL-Delivery id, for the receiver to drop
duplicates). A merchant can always read the same state from GET /v1/customers/{id} (usage.alerts).

    RL_REDIS=127.0.0.1:7001 control/.venv/bin/python -m rlcontrol.notifier     (from control/)
"""
import concurrent.futures
import hashlib
import json
import logging
import os
import threading
import time

import httpx
from redis.cluster import RedisCluster

from . import store, webhooks

log = logging.getLogger("rlcontrol.notifier")


def delivery(message):
    """The payload for one alert message, and its delivery id (stable across retries)."""
    alert = json.loads(message)
    merchant, sep, customer = alert["customer"].partition("|")
    if not sep:
        return None, None, None  # a customer of the single-owner mode has no merchant to tell
    payload = {"type": alert["event"], "merchant": merchant, "customer": customer, "used": alert["used"],
               "overage_used": alert["overage_used"], "limit": alert["limit"], "period": alert["period"],
               "at_ms": alert["at_ms"]}
    did = hashlib.sha256(f"{alert['customer']}|{alert['event']}|{alert['period']}".encode()).hexdigest()[:24]
    return merchant, payload, did


class Notifier:
    def __init__(self, redis, attempts=3, backoff_s=0.5, timeout_s=3.0, workers=8, allow_private=None):
        self.redis = redis
        self.attempts, self.backoff_s, self.timeout_s = attempts, backoff_s, timeout_s
        self.allow_private = webhooks.private_allowed() if allow_private is None else allow_private
        self.pool = concurrent.futures.ThreadPoolExecutor(workers, thread_name_prefix="webhook")
        self.stop = threading.Event()
        self.delivered = []  # (delivery id, status) of the last deliveries, for tests and logs

    def handle(self, message):
        merchant, payload, did = delivery(message)
        if merchant is None:
            return None
        hook = self.redis.hgetall(store.webhook_key(merchant))
        if not hook.get("url"):
            return None
        return self.pool.submit(self.send, hook["url"], hook["secret"], payload, did)

    def send(self, url, secret, payload, did):
        body = json.dumps(payload, separators=(",", ":")).encode()
        for attempt in range(1, self.attempts + 1):
            try:
                # Again (DNS may have changed since registration), and to the address checked, never
                # to a second resolution of the name.
                target, host, extensions = webhooks.pinned(url, webhooks.check_url(url, self.allow_private))
                headers = {"Content-Type": "application/json", "X-RL-Event": payload["type"], "X-RL-Delivery": did,
                           "X-RL-Signature": webhooks.sign(secret, body), **host}
                with httpx.Client(timeout=self.timeout_s, follow_redirects=False) as client:
                    status = client.post(target, content=body, headers=headers, extensions=extensions).status_code
                if 200 <= status < 300:
                    self.delivered.append((did, status))
                    log.info("delivered %s %s to %s", payload["type"], did, url)
                    return status
                if 400 <= status < 500 and status != 429:
                    self.delivered.append((did, status))
                    log.warning("refused %s %s by %s: %s", payload["type"], did, url, status)
                    return status  # the receiver says no: retrying will not change it
            except webhooks.UnsafeUrl as e:
                log.warning("not sending %s to %s: %s", did, url, e)
                return None
            except httpx.HTTPError as e:
                log.info("attempt %d of %s to %s failed: %s", attempt, did, url, e)
            if attempt < self.attempts:
                self.stop.wait(self.backoff_s * 2 ** (attempt - 1))
        self.delivered.append((did, None))
        log.warning("gave up on %s %s to %s", payload["type"], did, url)
        return None

    def run(self):
        """Subscribes to rl-alerts until stopped; resubscribes after a Redis error."""
        while not self.stop.is_set():
            try:
                pubsub = self.redis.pubsub()
                pubsub.subscribe(store.ALERTS)
                while not self.stop.is_set():
                    m = pubsub.get_message(ignore_subscribe_messages=True, timeout=0.5)
                    if m and m.get("type") == "message":
                        try:
                            self.handle(m["data"])
                        except (ValueError, KeyError) as e:
                            log.warning("malformed alert %r: %s", m["data"], e)
                pubsub.close()
            except Exception as e:  # noqa: BLE001: a lost connection must not end the notifier
                log.warning("subscription lost (%s); again in 1 s", e)
                self.stop.wait(1.0)

    def close(self):
        self.stop.set()
        self.pool.shutdown(wait=True)


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    host, port = os.environ.get("RL_REDIS", "127.0.0.1:7001").rsplit(":", 1)
    notifier = Notifier(RedisCluster(host=host, port=int(port), decode_responses=True))
    try:
        notifier.run()
    except KeyboardInterrupt:
        pass
    finally:
        notifier.close()


if __name__ == "__main__":
    main()
