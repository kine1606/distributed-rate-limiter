#!/usr/bin/env python3
"""Seeds a fresh merchant for tests/eval/portal_ui.mjs through a running management API: plans free and
pay, 30 customers cust-00..cust-29 (every third on pay) with usage counted by the real lua/quota.lua, and
cust-04 suspended. Prints the merchant; writes its management key as JSON.

    RL_ADMIN_TOKEN=<the API's operator token> control/.venv/bin/python tests/eval/portal_seed.py <out.json> [api url]
The API's Redis must be the THROWAWAY dev cluster (127.0.0.1:7001), which this writes to directly.
"""
import json
import os
import pathlib
import random
import sys

import httpx
from redis.cluster import RedisCluster

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "control"))
from rlcontrol import store  # noqa: E402

api = httpx.Client(base_url=sys.argv[2] if len(sys.argv) > 2 else "http://127.0.0.1:8765")
r = RedisCluster(host="127.0.0.1", port=7001, decode_responses=True)
merchant = "portal" + str(random.randrange(10 ** 6))  # a fresh merchant each time
created = api.post("/v1/merchants", json={"id": merchant},
                   headers={"Authorization": f"Bearer {os.environ['RL_ADMIN_TOKEN']}"})
created.raise_for_status()
auth = {"Authorization": f"Bearer {created.json()['management_key']}"}
api.put("/v1/plans/free", json={"daily": 200, "burst": 20, "refill": 5}, headers=auth).raise_for_status()
api.put("/v1/plans/pay", json={"daily": 2000, "burst": 100, "refill": 50, "overage": "postpaid", "allowance": 5000,
                               "fee": 1900, "overage_price": 50, "topup_price": 40}, headers=auth).raise_for_status()
quota = r.register_script((ROOT / "lua/quota.lua").read_text())
rng = random.Random(7)
for i in range(30):
    cid, plan = f"cust-{i:02d}", "pay" if i % 3 == 0 else "free"
    api.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth).raise_for_status()
    limit, policy, extra = (2000, "cap", 5000) if plan == "pay" else (200, "hard", 0)
    for _ in range(rng.randint(0, int(limit * 1.3)) // 10):  # calls of 10 units, as the limiter would count them
        quota(keys=[store.quota_key(merchant, cid)], args=[limit, 10, policy, extra, "day", 0, 0, 0, ""])
api.post("/v1/customers/cust-04/suspend", headers=auth).raise_for_status()
pathlib.Path(sys.argv[1]).write_text(json.dumps({"merchant": merchant, "key": created.json()["management_key"]}))
print("seeded", merchant)
