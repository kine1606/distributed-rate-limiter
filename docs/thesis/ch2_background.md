# Chapter 2. Background and related work

> Draft, Week 2 (2026-10-18). Target length 8 to 10 pages. Citations refer to `references.md`;
> entries marked L there still need their bibliographic details checked.

This chapter introduces what the design in Chapter 3 builds on: the algorithms used to limit
request rates (2.1), the properties of Redis that make an exact distributed counter possible
(2.2), the gRPC and Protocol Buffers stack that carries every check (2.3), and the existing systems
that solve related problems (2.4). Section 2.5 states what those systems leave open.

## 2.1 Rate limiting algorithms

A rate limiter decides, for each arriving request of a client, whether the request fits within a
limit of the form "at most L units per period T". The algorithms differ in what they remember and
therefore in how they behave at the edges of a period.

### 2.1.1 Fixed window counter

Time is cut into consecutive windows of length T aligned to a fixed origin (for example the Unix
epoch), and each client has one counter per window. A request is admitted if the counter is below
L, and the counter is incremented. State is one integer per client, and the counter can simply
expire with its window.

The cost of this simplicity is the window edge. A client that sends L requests at the end of one
window and L more at the start of the next has sent 2L requests in an interval of a few
milliseconds, all of them admitted. The project's script tests reproduce this directly:
`WindowEdgeBurstFixedAdmitsTwiceTheLimitSlidingDoesNot` sends L = 10 requests 150 ms before a
one-second window boundary and 10 more 20 ms after it: the fixed window admits all 20 within about
170 ms, the sliding window (2.1.2) at most one of the second ten.

### 2.1.2 Sliding window

A sliding window log stores the timestamp of every admitted request and admits a new one if fewer
than L timestamps fall within the last T. It is exact but needs memory proportional to L per
client, which is prohibitive for large limits.

The sliding window counter approximates the log with two fixed-window counters, the current and
the previous one. The number of requests in the last T is estimated as the current count plus the
previous count weighted by the fraction of the previous window that still overlaps the sliding
interval. Cloudflare describes this approximation for its rate limiting, noting that it assumes a
constant rate during the previous window and "in practice it proved to be good enough" [8]. State is
two integers per client, and the 2L edge burst of the fixed window disappears, at the price of a
small error when traffic within the previous window was uneven.

### 2.1.3 Token bucket

A token bucket has a capacity b and a refill rate r. It starts full; each request of cost c takes c
tokens if at least c are present and is denied otherwise; tokens are added continuously at rate r up
to the capacity. Over any interval of length t the bucket admits at most b + r·t units, so it allows
a burst of size b after a quiet period and a sustained rate r otherwise. The same structure is used
in network traffic conditioning, where RFC 2697 defines a marker based on token buckets with a
committed and an excess burst size [19], and Amazon API Gateway describes its request throttling in
terms of a token bucket with a rate and a burst [2].

A token bucket needs only two numbers of state per client, the token count and the time of the last
refill: the tokens due since then can be computed when the next request arrives instead of by a
timer. Two implementation details matter for a distributed limiter and recur in Chapter 3. First,
"now" must come from one clock for every instance; otherwise two instances with skewed clocks
compute different refills for the same bucket. Second, repeated fractional refills accumulate
floating-point error unless the state is kept in integers, for example in thousandths of a token.

The leaky bucket, in which requests drain from a queue at a fixed rate, enforces a smooth output
rate rather than a burst allowance. It suits traffic shaping more than API quotas, where a
customer's burst within their allowance should be served immediately rather than delayed.

### 2.1.4 The daily quota as a calendar window

A daily quota is a fixed window whose length is one day and whose origin is midnight in an agreed
time zone. It is the unit customers buy: "200 calls per day" is easy to state in a price list,
easy to show in a response header, and easy to reconcile with a bill. Its edge behaviour, the 2L
burst of 2.1.1, is acceptable for a quota because what the customer pays for is the count per day,
not the shape of traffic within it; the shape is the job of the second limit.

### 2.1.5 Why a quota plus a token bucket fits the business model

The two limits answer different questions and tolerate different errors. The **quota** answers "has
this customer used what they paid for today?". It is small (hundreds or thousands per day for most
customers), it is money, and it must be exact. The **burst limit** answers "is this customer
calling too fast right now?". It protects the provider's backend and other customers, it is checked
on every call at the full request rate, and an error of a few tokens is harmless as long as it is
bounded. A token bucket is the natural burst limit because it needs constant state, allows the short
bursts that real clients produce, and is cheap to evaluate. Using the two together lets the design
spend exactness only where it is needed, which is the central decision of Chapter 3.

## 2.2 Redis

Redis is an in-memory data store that keeps all data in RAM and serves it over a simple
request/response protocol. Four of its properties carry the design: command execution on a single
thread, atomic Lua scripts, a cluster mode that partitions keys by hash slot, and asynchronous
replication.

### 2.2.1 Single-threaded execution and atomic scripts

Redis executes commands one at a time on a single thread (later versions can use extra threads for
socket I/O, but not for executing commands). Every command is therefore atomic with respect to every
other. A check that has to read a counter, compare it with a limit and write it back is not a single
command, though, and doing it as separate commands from a client would let two clients read the same
value and both write an increment.

Lua scripts close that gap. A script runs inside the server, and "Redis guarantees the script's
atomic execution. While executing the script, all server activities are blocked during its entire
runtime" [11]. A read-compare-write of a token bucket or a quota, written as one script, therefore
behaves as one indivisible operation however many limiter instances call it. The same blocking is a
constraint: a slow script blocks every client of that Redis node, so scripts must do constant work,
with no loops over data and no scans.

Two rules of the scripting model shape the client code. First, the server keeps compiled scripts in
a cache indexed by the SHA1 digest of their source, and a client invokes a cached script with
`EVALSHA` and the digest instead of sending the source with `EVAL` each time. The cache "is always
volatile": it is not persisted and may be cleared on restart, on failover to a replica, or by
`SCRIPT FLUSH`, in which case `EVALSHA` returns a `NOSCRIPT` error [11]. A client must therefore be
prepared to fall back to `EVAL` with the source, which runs the script and caches it again. Second,
"all names of keys that a script accesses must be explicitly provided as input key arguments" [11];
a script that builds a key name internally works on a single server but breaks under cluster mode,
because the cluster routes the call by its declared keys.

**Time inside a script.** A limiter needs "now" inside the script, both for token refills and for
the current day of a quota. Redis offers the `TIME` command, which returns the server's clock. When
scripts were replicated by re-running them on replicas, a non-deterministic command such as `TIME`
before a write was forbidden, because a replica would compute a different result. Since Redis 5.0
scripts are replicated by their effects, the write commands they produced, and since Redis 7.0 that
is the only mode, so a script may call `TIME` freely [11]. Taking the time from the Redis node that
owns the key gives every limiter instance the same clock for that key.

**Pipelining.** A client may send several commands without waiting for each reply and read the
replies afterwards in order. One network round trip then carries many commands, so throughput is no
longer bounded by one round trip per call. The project's client spike (`docs/DESIGN.md`, section 3)
measured about 290,000 script calls per second from one sending thread over one connection per
master, against a three-master cluster on one laptop, by keeping calls in flight instead of waiting
for each.

### 2.2.2 Redis Cluster

Redis Cluster partitions the key space across several master nodes. The key space is split into
16,384 hash slots, and a key's slot is `CRC16(key) mod 16384`; each master owns a subset of the
slots [12]. A client that sends a command to the wrong master receives a `MOVED` reply naming the
owner, and updates its slot map; during a live migration of a slot it may receive `ASK`, a one-time
redirection [12].

Commands that touch several keys, including a script with several keys, work only if all the keys
are in the same slot. **Hash tags** make that controllable: if a key contains a substring in braces,
only that substring is hashed [12]. Keys such as `{cust:42}:plan`, `{cust:42}:daily_quota:q` and
`{cust:42}:burst:tb` all hash `cust:42`, so all of one customer's state lives on one master and one
script may read the plan and update the quota atomically. Distributing customers over masters
remains automatic, because different customers have different tags. The cost is that one very hot
customer cannot be spread over several masters; Chapter 3 addresses that case with leases.

### 2.2.3 Replication and failover

Each master can have replicas that receive its writes asynchronously. If a master fails, the
cluster promotes one of its replicas after the node timeout. The specification is explicit about the
consequence: asynchronous replication leaves "small windows where acknowledged writes can be lost"
[12], because a master may acknowledge a write and fail before forwarding it. For a quota counter,
this means a failover can lose a few increments, letting a customer make a few more calls than
counted; Chapter 3 treats this as a limitation and points to the usage event log as the durable
record for billing.

The project's spike also observed what happens without replicas. When one of three masters was
stopped, calls for its slots failed at once; after the node timeout, the cluster state became
`fail` and every master, including the two healthy ones, answered `CLUSTERDOWN` (with the default
`cluster-require-full-coverage yes`); after a restart the node came back without its script cache,
so its keys returned `NOSCRIPT` until the client fell back to `EVAL` (`docs/DESIGN.md`, section 3).
These observations define the failure cases Chapter 3 must handle.

## 2.3 gRPC and Protocol Buffers

The limiter is called by API gateways over gRPC, a remote procedure call framework that carries
Protocol Buffers messages over HTTP/2.

**HTTP/2** multiplexes many concurrent requests as independent streams over one TCP connection
[16], so a gateway can keep thousands of checks in flight to a limiter instance without opening
thousands of connections, and a slow check does not block the ones behind it. A lost TCP packet
still delays every stream on the connection, because HTTP/2 does not address head-of-line blocking
in TCP itself [16].

**Protocol Buffers** define the request and response messages in a schema compiled into C++ and
into any client language [15]. Two properties of the format shape the API in Chapter 3. Each field
is identified on the wire by its number, not its name, so field numbers must never be reused or
renumbered once clients exist; removed fields are declared `reserved`. And in proto3 a field that was
never set reads as its zero value, so an enumeration whose zero value meant "allowed" would turn a
server bug that forgets to set the reason into an apparent success; the zero value is therefore
reserved for "unspecified".

**Status codes, deadlines and retries.** A gRPC call ends with a status code such as `OK`,
`INVALID_ARGUMENT`, `RESOURCE_EXHAUSTED` or `UNAVAILABLE` [14]. A client sets a deadline per call,
after which the call fails with `DEADLINE_EXCEEDED` instead of waiting indefinitely [14]. Clients can
be configured with a retry policy that retries a call, with exponential backoff, when it fails with
one of a list of retryable status codes [13]; `UNAVAILABLE` is the code such policies usually list.
This matters for the API design: if the limiter reported "Redis is down" as `UNAVAILABLE`, retry
policies would repeat the call against a store that is already struggling. The API in Chapter 3
therefore reports a decision, including "denied because Redis is unreachable", inside an `OK`
response, and reserves non-`OK` statuses for calls the limiter could not decide at all.

**The C++ callback API.** gRPC's C++ server offers an asynchronous callback API in which a handler
returns immediately with a reactor object and completes the call later, from any thread, by calling
`Finish` exactly once. A check that has to wait for Redis does not occupy a server thread while it
waits; it finishes from the Redis client's I/O thread when the reply arrives, and a check served from
a local lease finishes inline.

## 2.4 Related systems

Table 2.1 compares existing systems that enforce request limits. The comparison uses four
properties: whether a configured limit is enforced exactly across instances, what a check costs on
the hot path, how the system is deployed, and whether its source is open.

*Table 2.1: Rate limiting systems compared (from each system's documentation, accessed October 2026).*

| System | Exact across instances? | Hot-path cost of a check | Deployment | Source |
| --- | --- | --- | --- | --- |
| Amazon API Gateway usage plans [1, 2] | No: quotas and throttles are "applied on a best-effort basis" | inside the managed gateway | managed AWS service | proprietary |
| Kong rate limiting plugin [3, 4] | `local`: no; `cluster`: yes (datastore read and write per request); `redis`: only with `sync_rate` 0 | local memory, datastore, or Redis per request | plugin in the Kong gateway | open source (Redis Cluster support in Enterprise) |
| Envoy ratelimit service [5] | Per Redis counter; fixed windows of a second, minute, hour or day | one gRPC call to the service, which updates Redis; a local cache of over-limit keys avoids Redis for denials | separate Go service called by Envoy | open source |
| Unkey [6] | Converges: regional decisions, global convergence within seconds (vendor statement) | regional check, ~30 ms decisions stated by the vendor | managed service, self-hostable | source available, AGPL-3.0 |
| Upstash Ratelimit [7] | Per Redis counter; fixed window recommended for multi-region | client library call to Redis per check | library over (serverless) Redis | open-source SDK |
| Doorman [9] | Cooperative: clients receive capacity leases and are trusted to respect them | local, within the leased capacity | separate Go service, client libraries | open source, archived 2024 |
| Gubernator [20] | Exact at each limit's owner peer; `GLOBAL` mode trades consistency for scale | forward to the owner peer, batched; with `GLOBAL`, local with asynchronous sync | Go service of peers, state in memory, no Redis | open source (Apache-2.0) |
| Bucket4j [21] | Exact by default; the `delaying` optimization may over-consume up to a per-node token threshold | remote store per check, or local until the threshold | Java library over Redis, JCache or a database | open source (Apache-2.0) |
| This thesis | Quota: yes (one atomic script per check). Burst: bounded by N × C, measured | quota: one Redis round trip; burst: one compare-and-swap, Redis only for refills | gRPC service with Redis Cluster | open source (this repository) |

**Amazon API Gateway** attaches usage plans with a throttle (rate and burst) and a quota (requests
per day, week or month) to API keys. The documentation states that both "are not hard limits, and
are applied on a best-effort basis", that "clients can exceed the quotas that you set", and
recommends AWS Budgets and AWS WAF instead of usage plans for controlling costs or access [1]. It is
the clearest statement of the gap this thesis addresses: a widely used managed product chooses not
to promise exact quotas.

**Kong** offers three counter policies with an explicit accuracy trade-off [3]. `local` keeps
counters in each node's memory, which is fast but diverges as nodes are added; `cluster` stores
counters in Kong's datastore with a read and a write per request, which is accurate but has the
largest performance cost; `redis` shares counters through Redis and is accurate only when
synchronised on every request (`sync_rate` 0). Kong's guidance rules out `local` where every
transaction matters, such as financial transactions [3]. The trade-off is the same one this thesis
faces, but Kong chooses one policy per plugin, while this thesis uses the exact path and the fast
path for different limits of the same call.

**Envoy's ratelimit service** is a separate gRPC service that Envoy proxies call with a domain and a
set of descriptors; it keeps counters in Redis with limits per second, minute, hour or day [5]. Two of
its features have counterparts in this design: an optional local cache of keys already over their
limit, which avoids reading Redis again for a client that is being denied (the deny cache in Chapter
3), and a `near_limit` statistic counting requests above a configurable ratio of the limit, 0.8 by
default (the 80% warning described for the customer lifecycle). Every allowed request still costs a
Redis update. Measured on the same laptop with the same load generator, the service handled 38,167
checks per second with one Redis update each, against 75,652 for this thesis's leased path
(`docs/thesis/evidence/ceiling_and_envoy_2026-10-19.md`).

**Unkey** and **Upstash Ratelimit** are recent services aimed at API businesses. Unkey implements a
sliding window and makes decisions in the region nearest the caller; its documentation describes
global convergence within seconds for identifiers approaching their limit, which means a small
overshoot across regions is possible [6]. Upstash provides fixed window, sliding window and token
bucket algorithms as a client library over its Redis service, and recommends the fixed window for
multi-region use because the sliding window then needs many Redis commands per check [7]. Both treat
exactness across regions as eventual, and neither separates an exact quota from an approximate burst
limit.

**Gubernator** and **Bucket4j** show that serving checks locally and synchronizing later is a common
answer to the cost of a shared store. Gubernator, developed at Mailgun, keeps limits in the memory of
a cluster of peers and sends each check to the limit's owner, chosen by consistent hashing, in
batches; its `GLOBAL` mode answers on the receiving peer and sends the hits to the owner
asynchronously, which its documentation says "allows for greater scale but at the cost of
consistency" [20]. Bucket4j, a Java library, offers a `delaying` optimization that serves requests
locally until a number of unsynchronized tokens or a time limit is reached, and a `predicting`
variant meant to reduce the over-consumption that delaying causes [21]. Neither states a bound on the
over-admission across instances; Bucket4j's per-node threshold has the same form as the N × C bound
derived in Chapter 3.

**Doorman**, developed at YouTube, is the closest precedent for the lease half of this design [9].
Clients ask a Doorman server for capacity, receive a share as a time-limited lease that they refresh
periodically, and limit themselves locally within it; servers divide capacity according to demand.
The approach removes the shared store from the hot path, but it relies on clients to respect their
leases. This thesis applies the same idea inside a trusted server-side limiter: the instances that
hold leases are the limiter itself, so the leased tokens are enforced, and the over-admission they
allow is bounded and measured rather than assumed. The term goes back to Gray and Cheriton, who
define a lease as "a contract that gives its holder specified rights over property for a limited
period of time" and use the time limit so that host and communication failures cost performance
rather than correctness [10].

## 2.5 Summary

Rate limiting algorithms trade memory for accuracy at window edges; a daily quota is a fixed window
because it is what customers buy, and a token bucket is the natural burst limit. Redis makes an exact
counter possible through atomic scripts, the Redis clock and hash-tagged keys, at the cost of one
round trip per check and of a small window of lost writes on failover. gRPC provides the transport,
and its retry semantics argue for reporting a store outage as a decision rather than an error.

The systems in Table 2.1 each sit at one point of the trade-off between exactness and cost: managed
gateways and multi-region services accept approximate quotas, and the exact options pay a store
round trip on every request. None enforces an exact per-customer quota and serves the high-volume
burst check locally with a stated, measured bound on its error. That combination, and its evaluation,
is the subject of the following chapters.
