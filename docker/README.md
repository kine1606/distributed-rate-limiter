# Docker environment

> Week 1 (cluster up, host access), Week 5 (fault injection), Week 6 (multi-instance tests) · Status: Week 1 done: base cluster verified (`cluster_state:ok`, hash-tagged keys in one slot); development profile `docker-compose.dev.yml` reachable from WSL. Week 2: the 3 limiters run on the cluster (image builds redis-plus-plus), and share one daily quota across instances

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

While developing, you run `rate_limiter_server`, the spike and the tests directly in WSL, not in a
container. A cluster node tells clients the address of the node that owns a slot
(`MOVED 1234 172.18.0.3:6379`), and a container address is not reachable from WSL, so the nodes
must announce an address WSL can use.

`docker-compose.dev.yml` is a standalone file (its own project, `ratelimiter-dev`), not an override:

```bash
docker compose -f docker/docker-compose.dev.yml up -d
redis-cli -c -p 7001 cluster info          # cluster_state:ok, from WSL
redis-cli -p 7001 cluster nodes            # 127.0.0.1:7001-7003, three masters
redis-cli -p 6380 ping                     # plain throwaway Redis for the Lua script tests
```

How it works: redis-2, redis-3 and the init container join redis-1's network namespace
(`network_mode: service:redis-1`), and redis-1 publishes 7001 to 7003. All three nodes then share
one loopback, so the `127.0.0.1` they announce is right both for each other (cluster bus) and for
clients in WSL. Announcing `127.0.0.1` from separate containers breaks the bus, because each
container's loopback is its own; that is also why this cannot be an override of the base file
(`network_mode` and `networks` cannot be combined). Consequence for fault tests in this profile:
stop redis-2 or redis-3; stopping redis-1 takes the shared network down with it. The base file has
separate containers, so any node can be killed there.

The init containers (both files) create the cluster only if `cluster_state` is not already `ok`, so
`up` can be run again without failing on "node is not empty", and exit only once the cluster has
converged, so the limiters never start against a cluster that still answers `CLUSTERDOWN`.

The dev cluster and the plain Redis on 6380 replace port 6379 for everything that writes or flushes
test data (`bench/ghz_run.sh` flushes the cluster), so a
system Redis on 6379 is never touched.

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

- [x] Week 1: `cluster_state:ok` and hash-tagged keys land in one slot (`{cust:42}:quota` and
      `{cust:42}:tb` both in slot 3057).
- [x] Week 1: `redis-cli -c -p 7001 cluster info` works from WSL (development profile).
- [ ] Week 5: kill and latency tests scripted in `tests/eval/`.
