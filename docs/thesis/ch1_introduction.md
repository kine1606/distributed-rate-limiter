# Chapter 1. Introduction

> Draft, Week 2 (2026-10-18). Target length 4 to 6 pages. Statements marked **[Pending: Week N]**
> describe results that do not exist yet; they must be replaced by measured numbers, or removed,
> before submission. Citations refer to `references.md`.

## 1.1 Context: selling access to APIs

A growing share of software is sold not as an installed product but as an API that customers call
over the network. Providers of large language models, SMS and one-time-password gateways, maps,
payments and market data all follow the same commercial pattern: a customer signs up, receives an
API key, chooses a plan, and pays for a quantity of calls. A free plan might allow 200 calls per
day, a paid plan 2,000, an enterprise contract a million. On top of the daily quantity, most plans
also limit how fast calls may arrive, for example 2 calls per second on the free plan, both to
protect the provider's backend and to stop one customer's burst from degrading service for the
others.

Every call therefore raises a question that must be answered before the call is served, in real
time and on the request path: *has this customer already used what they paid for?* The component
that answers it is a rate limiter, or more precisely a quota service: it holds each customer's
usage, compares it with the customer's plan, and returns allow or deny. Because it sits in front of
every API call, its answer has two properties that pull in opposite directions. It must be correct,
because the quota is the thing being sold. And it must be cheap, because it is paid on every call,
including the vast majority of calls from customers who are nowhere near their limit.

## 1.2 Problem

**Correctness.** A quota is a promise in both directions. If the limiter admits more calls than the
plan allows, the provider gives away the product it sells; if it admits fewer, it blocks a paying
customer, who sees errors for service they paid for. Exactness is harder than it looks once the
service runs as several instances behind a load balancer. Two instances that each read "199 used"
and each admit one more call have admitted 201. A client that retries a call after a timeout must
not be charged twice. A customer who upgrades at noon expects the higher limit on the next call, not
after a cache expires. And every instance must agree on when "today" ends, or a customer can be
reset twice or not at all.

Widely used managed gateways do not promise exactness. Amazon API Gateway documents that "usage
plan throttling and quotas are not hard limits, and are applied on a best-effort basis. In some
cases, clients can exceed the quotas that you set" [1], and advises against relying on them to
control costs. Kong's rate limiting plugin offers a `local` policy that keeps counters in each
node's memory and, unless a consistent-hashing load balancer sits in front, "diverges when scaling
the number of nodes", and a `redis` policy that is accurate only when its counters are
synchronised on every request (`sync_rate` set to 0) [3]. Approximation is a reasonable choice for
protecting a backend from overload; it is not a reasonable way to enforce a plan a customer pays for.

**Throughput.** The obvious way to be exact is to keep one counter per customer in a shared store
and update it atomically on every call. Every check then costs a network round trip to the store,
and the store sees every API call of every customer. At the scale of a large API platform, on the
order of a million checks per second, that round trip dominates both the latency added to each API
call and the cost of running the limiter. The store becomes the bottleneck, and the customers who
make most of the traffic, those far below their limits, pay the full price of exactness they do not
need on most of their calls.

The problem this thesis addresses is how to be **exact where it matters, the daily quota, while
being fast where it matters, the high-volume burst check**, and how to make the price of the speed,
a bounded inaccuracy, explicit and measured rather than unknown.

## 1.3 Goals

The goals come from the two halves of the problem (`docs/DESIGN.md`, section 1).

| Goal | Statement |
| --- | --- |
| G1 | No over-admission of a daily quota, under concurrent calls to any number of limiter instances. |
| G2 | A retried call (same request id within five minutes) is counted once. |
| G3 | A plan change applies on the customer's next call, keeping the day's usage. |
| G4 | One reset moment for every customer and instance: 00:00 UTC by the Redis clock. |
| G5 | About 1,000,000 checks per second across a cluster, reached by adding instances and Redis masters. |
| G6 | Server-side latency per check: p50 below 500 µs, p99 below 2 ms. |
| G7 | Over-admission of the burst limit bounded by N × C per refill interval (N instances, lease chunk C), and measured. |
| G8 | Over-quota floods refused locally; an overloaded instance sheds load explicitly. |
| G9 | Documented behaviour when Redis is slow or unavailable, per plan. |

G5 is a cluster-wide figure that a single laptop cannot produce. It is reached in this thesis by
measuring the throughput of one instance and of one Redis master, and extrapolating with every
assumption stated (Chapter 5); it is never reported as a measurement. Billing, account management,
user interfaces and multi-region deployment are out of scope: the limiter enforces plans, it does
not sell them.

## 1.4 Approach

The thesis builds a quota and rate limiting service in C++20, gRPC and Redis Cluster that splits the
problem along the line between the two halves.

The **daily quota** is checked strictly: every call runs one Lua script in Redis that reads the
customer's usage for the current UTC day, compares it with the limit from the customer's current
plan, and increments it, atomically. Redis runs a script to completion before any other command
[11], so two limiter instances can never both take the last unit of a quota. All of one customer's
keys share a Redis Cluster hash tag and therefore live on one master [12], which keeps each check to
a single round trip to a single node while the cluster as a whole scales out.

The **burst limit** is served from local leases. Each limiter instance takes a chunk of a
customer's token bucket from Redis and hands it out with lock-free atomic operations in its own
memory; only when the chunk runs low does it ask Redis for another, in a pipelined refill.
A burst check that finds tokens costs one compare-and-swap and no network round trip. The price is
that tokens held by one instance are invisible to the others: with N instances and chunk size C, at
most N × C tokens can be outside Redis, which bounds the over-admission per refill interval. The
idea of handing out capacity as leases goes back to Gray and Cheriton's leases for cache consistency
[10] and appears in production rate limiting in YouTube's Doorman [9]; this thesis applies it to a
server-side token bucket and measures the resulting bound.

A server-side composite check combines the two for one API call: suspended accounts and customers
already known to be over quota are refused locally, the burst limit is checked next, and only calls
that pass the burst check reach the strict quota script. In the version built, a customer's burst
limit is still one script per check; leasing it per customer is future work (Section 3.2).

## 1.5 Contributions

1. **A quota design that is exact under concurrency, retries and plan changes.** Daily counters keyed
   by the UTC day taken from the Redis clock, storing `used` rather than `remaining` so that plan
   changes rewrite nothing, with request-id deduplication and three overage policies (hard, grace,
   capped billable overage). A Free customer is stopped at exactly 200 of 400 concurrent calls through
   gRPC on a three-master cluster (measured in Week 2). A retried call is counted once, and request
   2,001 of a plan of 2,000 a day gets three different answers under the three overage policies,
   tested through gRPC (Week 4).
2. **A lease-based fast path with a derived and measured accuracy bound.** Cache-line-aligned leases
   in a lock-free table with single-flight refills, chunk sizes capped at a fraction of the limit,
   and pipelined Redis refills. The bound N × C is derived in Chapter 3 and measured there: on four
   instances at chunk caps of 1%, 5% and 10% the worst case stays below it, as a model of the
   protocol predicts to within 0.3 tokens (Week 6).
3. **An evaluation that tests each half of the claim separately.** Exactness tests (concurrency,
   day rollover, plan changes, overage), then performance and robustness tests (strict against
   leased mode, scaling across instances, floods, key cardinality, Redis failures), with
   reproducible scripts and raw results. A first strict baseline exists (Week 2: a median of 25,301
   checks per second for one instance on one laptop at high concurrency, 10,024 at p99 10.5 ms);
   the full matrix is **[Pending: Weeks 6 to 8]**.

## 1.6 Structure of the thesis

Chapter 2 introduces the background: rate limiting algorithms, the Redis properties the design
relies on (single-threaded atomic scripts, cluster hash slots, failover), gRPC and Protocol Buffers,
and existing rate limiting systems compared in one table. Chapter 3 presents the design:
requirements, architecture, data model, quota and overage logic, the lease protocol with the
derivation of its accuracy bound, failure policies, and the capacity estimate for one million
checks per second. Chapter 4 describes the implementation: the Lua scripts, the Redis client and
its measured selection, the lease table, the gRPC server and the test approach. Chapter 5 evaluates
the system against each goal with measured results and their limitations. Chapter 6 concludes with
the results against the goals, the limitations, and future work.
