# Chapter 1. Introduction

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

Most API owners do not want to build that component, so it can itself be sold as a service. Three
parties then meet in it:
- **The platform** runs the service and its Redis cluster, and admits API owners.
- **A merchant** is an API owner that buys the service. It defines its own plans (Free, Pay,
  Enterprise, with its own numbers and prices), signs up its own customers, and calls the service from
  its API gateway before serving each call.
- **A customer** is one of the merchant's users, identified by an id the merchant chooses. It never
  calls the service directly; its calls reach it through the merchant's gateway.

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

**Many merchants.** Selling the limiter to many API owners adds four demands. One merchant must not
see, change or slow down another's customers, though they share the cluster. A merchant changes
limits while its customers are calling: an upgrade, a boost in an emergency, a suspension. It bills
its customers for what they used, so the numbers on an invoice must be the numbers the limiter
decided with, also when a Redis master fails. And a merchant's backend has a capacity of its own:
quotas sold per day can arrive in the same second, and when they do, the merchant would rather turn
away its free users than its paying ones.

The problem this thesis addresses is how to be **exact where it matters, the daily quota, while
being fast where it matters, the high-volume burst check**, and how to make the price of the speed,
a bounded inaccuracy, explicit and measured rather than unknown. Then, how to offer that limiter to
many merchants as a service, isolated from each other, with invoices that equal the metered usage.

## 1.3 Goals

The goals come from the two halves of the problem (`docs/DESIGN.md`, section 1); G10 to G14 come from
selling the service to many merchants (`docs/PRODUCT_PLAN.md`, section 7).

*Table 1.1: The goals of this thesis.*

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
| G10 | One merchant cannot read, change or slow another's limits. |
| G11 | A limit change or boost applies on every instance within 1 s. |
| G12 | An invoice equals the metered usage exactly, retries included. |
| G13 | Billed overage never exceeds the spending cap. |
| G14 | A merchant's backend never receives more than its capacity plus N × C, and paying customers keep their reserve under a flood of Free calls. |

G5 is a cluster-wide figure that a single laptop cannot produce. It is reached in this thesis by
measuring the throughput of one instance and of one Redis master, and extrapolating with every
assumption stated (Chapter 5); it is never reported as a measurement.

In scope for merchants are the management API, the merchant portal, invoices made from the metered
usage, and charging them through a payment provider's sandbox. Out of scope are a payment business
(taxes, refunds, collecting debts), multi-region deployment, and a customer-facing sign-up: the
merchant signs up its own customers.

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

**Merchants** are a layer around this core, not a change to it. A call carries its merchant's API
key; the service resolves it to the merchant from an in-memory index and names the customer
`<merchant>|<customer>` before the engines see it, so every key, lease and cached denial below is the
merchant's own. A control plane writes merchants' plans, limits and boosts to Redis and announces each
change, as the plan change above does. The quota script keeps each month's billable totals in the same
atomic step as the decision, and invoices are made from them. A merchant's capacity is one more token
bucket served from leases, split into a share for every plan and a reserve for plans with a fee.

## 1.5 Contributions

1. **A quota design that is exact under concurrency, retries and plan changes.** Daily counters keyed
   by the UTC day taken from the Redis clock, storing `used` rather than `remaining` so that plan
   changes rewrite nothing, with request-id deduplication and three overage policies (hard, grace,
   capped billable overage). A Free customer is stopped at exactly 200 of 400 concurrent calls through
   gRPC on a three-master cluster, and quotas were exact in 20 of 20 adversarial runs on four
   instances (5.2). A retried call is counted once, and request 2,001 of a plan of 2,000 a day gets
   three different answers under the three overage policies, tested through gRPC (5.3).
2. **A lease-based fast path with a derived and measured accuracy bound.** Cache-line-aligned leases
   in a lock-free table with single-flight refills, chunk sizes capped at a fraction of the limit,
   and pipelined Redis refills. The bound N × C is derived in Chapter 3 and measured there: on four
   instances at chunk caps of 1%, 5% and 10% the worst case stays below it, as a model of the
   protocol predicts to within 0.3 tokens (5.4).
3. **An evaluation that tests each half of the claim separately.** Exactness tests (concurrency,
   day rollover, plan changes, overage), then performance and robustness tests (strict against
   leased mode, scaling across instances, floods, key cardinality, Redis failures), with
   reproducible scripts and raw results. Every table of Chapter 5 is generated by a script from the
   raw results, and the estimate for 1,000,000 checks per second is derived from measured
   per-instance figures with every assumption written out (5.11).
4. **A multi-tenant service around the limiter, with metering in the decision.** Merchants isolated by
   name rather than by deployment; limits, boosts and capacities changed at run time; invoices made from
   totals the quota script keeps atomically with each decision; and a merchant's capacity with a reserve
   for paying customers, served from leases. Each is evaluated on the real limiter: isolation and a
   noisy merchant, the latency of a change, invoices against the counters with a master killed, the
   capacity under a Free flood, 10,000 merchants, and the cost of tenancy (5.12).

## 1.6 Structure of the thesis

Chapter 2 introduces the background: rate limiting algorithms, the Redis properties the design
relies on (single-threaded atomic scripts, cluster hash slots, failover), gRPC and Protocol Buffers,
and existing rate limiting systems compared in one table. Chapter 3 presents the design:
requirements, architecture, data model, quota and overage logic, the lease protocol with the
derivation of its accuracy bound, failure policies, and the capacity estimate for one million
checks per second, then merchants (3.9). Chapter 4 describes the implementation: the Lua scripts,
the Redis client and its measured selection, the lease table, the gRPC server, merchants and the
control plane, and the test approach. Chapter 5 evaluates the system against each goal with measured
results and their limitations, merchants in 5.12. Chapter 6 concludes with
the results against the goals, the limitations, and future work.
