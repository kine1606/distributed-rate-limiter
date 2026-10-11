# Docker environment

> Week 1 (cluster up, host access), Week 5 (fault injection), Week 6 (multi-instance tests) · Status: compose file with 3 masters and 3 limiters

## What this module does

It gives you a realistic small deployment on one laptop: a Redis Cluster with three masters and
several limiter instances. The integration and evaluation tests all run against it, so it must
start the same way every time.

## Week 1: bring the cluster up

The service names in `docker-compose.yml` are `redis-1`, `redis-2`, `redis-3` and
`redis-cluster-init`. The Redis nodes publish no ports, so run `redis-cli` inside a container:

```bash
docker compose -f docker/docker-compose.yml up -d redis-1 redis-2 redis-3 redis-cluster-init
docker compose -f docker/docker-compose.yml logs redis-cluster-init     # expect "[OK] All 16384 slots covered"
docker compose -f docker/docker-compose.yml exec redis-1 redis-cli cluster info    # cluster_state:ok
docker compose -f docker/docker-compose.yml exec redis-1 redis-cli cluster nodes   # 3 masters
docker compose -f docker/docker-compose.yml exec redis-1 redis-cli cluster keyslot '{cust:42}:quota'
docker compose -f docker/docker-compose.yml exec redis-1 redis-cli cluster keyslot '{cust:42}:tb'
# same number twice: the hash tag puts both keys in one slot
docker compose -f docker/docker-compose.yml exec redis-1 redis-cli -c set '{cust:42}:x' 1   # -c follows MOVED
```

Hint: if `cluster info` says `fail`, the init container probably ran before all three nodes were
ready. Run it again (`docker compose ... up redis-cluster-init`) and read its logs.

## Week 1 task: reach the cluster from WSL (outside Docker)

While developing, you will run `rate_limiter_server`, the spike and the tests directly in WSL, not
in a container. That needs two changes, because a cluster node tells clients the address of the
node that owns a slot (`MOVED 1234 172.18.0.3:6379`), and the container address may not be
reachable from WSL.

Add a development profile with published ports and announced addresses, one per node:

```yaml
# docker-compose.dev.yml (HINT: use as an override file:
#   docker compose -f docker/docker-compose.yml -f docker/docker-compose.dev.yml up -d)
services:
  redis-1:
    ports: ["7001:7001", "17001:17001"]
    command: >
      redis-server --port 7001 --cluster-enabled yes --cluster-config-file nodes.conf
      --cluster-node-timeout 5000 --appendonly no --save ""
      --cluster-announce-ip 127.0.0.1 --cluster-announce-port 7001 --cluster-announce-bus-port 17001
  # HINT: redis-2 on 7002/17002 and redis-3 on 7003/17003 the same way
  # HINT: the init command must then use the new ports: redis-1:7001 redis-2:7002 redis-3:7003
```

Then from WSL `redis-cli -c -p 7001 cluster info` works, and so does the C++ client with
`127.0.0.1:7001` as its seed. The Lua tests do not need the cluster; they use one plain Redis:
`docker run -d --name redis-test -p 6379:6379 redis:7`.

Hint: if announcing `127.0.0.1` breaks traffic between the nodes themselves, use your WSL IP
(`hostname -I`) instead, or run the development server inside the compose network. Whichever works,
write it down in this file, because the evaluation scripts will depend on it.

## Changes to make over the weeks

| Change | Why | Week |
| --- | --- | --- |
| Development profile with ports and announce addresses | Run the server and tests from WSL | 1 |
| Add one replica per master (`--cluster-replicas 1`, 6 nodes) | Failover tests need a replica to promote | 5 |
| Lower `cluster-node-timeout` to 3000 if failover feels slow | Faster fault tests | 5 |
| Add a Prometheus container and `prometheus.yml` | Metrics during load tests | 5 |
| Add toxiproxy (or use `tc netem`) in front of Redis | Latency and connection-drop tests | 5 |
| Scale limiters (`limiter-4`, or `--scale` with a port range) | Scaling and accuracy tests | 6 |

## Fault injection commands

```bash
docker compose -f docker/docker-compose.yml stop redis-1      # kill a master
docker compose -f docker/docker-compose.yml start redis-1     # it rejoins (as a replica if one was promoted)
docker compose -f docker/docker-compose.yml exec redis-1 tc qdisc add dev eth0 root netem delay 20ms
docker compose -f docker/docker-compose.yml exec redis-1 tc qdisc del dev eth0 root
docker network disconnect ratelimiter_rl ratelimiter-redis-2-1   # drop connections
```

`tc` needs the `NET_ADMIN` capability (`cap_add: [NET_ADMIN]`) and the `iproute2` package in the
image, which `redis:7` does not have. Toxiproxy avoids both and is easier to script.

Hint: a Docker network on a laptop has very low latency, which makes Redis look faster than it would
be across machines. Say so in the evaluation, and use the 20 ms latency test to show how the lease
hides network latency.

## Done when

- [ ] Week 1: `cluster_state:ok` and hash-tagged keys land in one slot.
- [ ] Week 1: `redis-cli -c -p 7001 cluster info` works from WSL (development profile).
- [ ] Week 5: kill and latency tests scripted in `tests/eval/`.
