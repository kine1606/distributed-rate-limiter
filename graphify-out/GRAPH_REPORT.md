# Graph Report - distributed-rate-limiter  (2026-10-11)

## Corpus Check
- Corpus is ~6,157 words - fits in a single context window. You may not need a graph.

## Summary
- 210 nodes · 318 edges · 14 communities (8 shown, 6 thin omitted)
- Extraction: 87% EXTRACTED · 13% INFERRED · 0% AMBIGUOUS · INFERRED: 40 edges (avg confidence: 0.84)
- Token cost: 58,047 input · 0 output

## Community Hubs (Navigation)
- Engineering Rules & Agents
- Lua Script Tests
- Server & Bench Wiring
- Check Request/Result Types
- Core gRPC Service
- Rule Model
- Config Tests
- Rule Config Loader
- vcpkg Manifest
- Build Script
- Test Script

## God Nodes (most connected - your core abstractions)
1. `TEST_F()` - 19 edges
2. `LuaTest` - 17 edges
3. `Rule` - 12 edges
4. `Lease manager (local fast path)` - 11 edges
5. `README: Distributed Rate Limiter` - 10 edges
6. `CheckRequest` - 9 edges
7. `RateLimiterService` - 9 edges
8. `Reply` - 9 edges
9. `cpp-pro subagent` - 9 edges
10. `CLAUDE.md project instructions` - 9 edges

## Surprising Connections (you probably didn't know these)
- `Thread safety: fine-grained locking or lock-free atomics` --semantically_similar_to--> `Hot path rules: no mutex, no heap alloc, no per-request logging`  [INFERRED] [semantically similar]
  AGENTS.md → .agents/agents/cpp-pro.md
- `Five build stages` --semantically_similar_to--> `Thesis plan (Oct 5 - Nov 20, 2026)`  [INFERRED] [semantically similar]
  README.md → docs/THESIS_PLAN.md
- `cpp-pro subagent` --references--> `RL_SANITIZER build option (asan/tsan)`  [INFERRED]
  .agents/agents/cpp-pro.md → CMakeLists.txt
- `Cache-line aligned per-shard data` --rationale_for--> `Lease manager (local fast path)`  [INFERRED]
  .agents/agents/cpp-pro.md → src/lease/README.md
- `CLAUDE.md project instructions` --references--> `Never present extrapolation as measurement`  [INFERRED]
  CLAUDE.md → .agents/agents/performance-engineer.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Check request path: gRPC server -> lease manager -> Redis backend -> Redis Cluster** — agents_ratelimiterserviceimpl, src_lease_readme, src_redis_readme, docker_docker_compose_redis_cluster, docs_design_architecture [INFERRED 0.85]
- **Agent instruction files sharing AGENTS.md rules** — claude, gemini, agents, _agents_agents_cpp_pro, _agents_agents_performance_engineer [EXTRACTED 1.00]
- **Lease vs strict accuracy/throughput trade-off** — readme_hybrid_lease_design, docs_design_accuracy_bound, _agents_agents_performance_engineer_strict_vs_leased_comparison, config_rules_login_attempts, config_rules_api_default [INFERRED 0.85]

## Communities (14 total, 6 thin omitted)

### Community 0 - "Engineering Rules & Agents"
Cohesion: 0.08
Nodes (38): cpp-pro subagent, gRPC callback API (CheckCallback, never block), Third-party Notice (wshobson/agents MIT), wshobson/agents repository, performance-engineer subagent, ghz gRPC load tool, Strict vs leased mode comparison across chunk sizes, AGENTS.md agent guidelines (+30 more)

### Community 1 - "Lua Script Tests"
Cohesion: 0.08
Nodes (11): Closer, LuaTest, ctx_, quota_sha_, tb_sha_, ReadFile(), Reply, err (+3 more)

### Community 2 - "Server & Bench Wiring"
Cohesion: 0.09
Nodes (3): BM_AllowAllCheck(), AllowAllCore, TEST()

### Community 3 - "Check Request/Result Types"
Cohesion: 0.10
Nodes (15): CheckRequest, cost, key, rule, CheckResult, allowed, limit, remaining (+7 more)

### Community 4 - "Core gRPC Service"
Cohesion: 0.13
Nodes (7): RateLimiterCore, BatchState, pending, reactor, Fill(), RateLimiterService, core_

### Community 5 - "Rule Model"
Cohesion: 0.13
Nodes (14): Algorithm, kSlidingWindow, kTokenBucket, FailurePolicy, kFailClosed, kFailOpen, Rule, algorithm (+6 more)

### Community 7 - "Rule Config Loader"
Cohesion: 0.29
Nodes (4): LoadRules(), ParseAlgorithm(), ParsePolicy(), main()

### Community 8 - "vcpkg Manifest"
Cohesion: 0.50
Nodes (3): dependencies, name, version-string

## Knowledge Gaps
- **36 isolated node(s):** `kTokenBucket`, `kSlidingWindow`, `kFailOpen`, `kFailClosed`, `name` (+31 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 95 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **6 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `TEST_F()` connect `Lua Script Tests` to `Server & Bench Wiring`?**
  _High betweenness centrality (0.094) - this node is a cross-community bridge._
- **Are the 10 inferred relationships involving `Lease manager (local fast path)` (e.g. with `Cache-line aligned per-shard data` and `Hot path rules: no mutex, no heap alloc, no per-request logging`) actually correct?**
  _`Lease manager (local fast path)` has 10 INFERRED edges - model-reasoned connections that need verification._
- **What connects `kTokenBucket`, `kSlidingWindow`, `kFailOpen` to the rest of the system?**
  _36 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Engineering Rules & Agents` be split into smaller, more focused modules?**
  _Cohesion score 0.07738095238095238 - nodes in this community are weakly interconnected._
- **Why does `LuaTest` connect `Lua Script Tests` to `Server & Bench Wiring`?**
  _High betweenness centrality (0.075) - this node is a cross-community bridge._
- **Should `Lua Script Tests` be split into smaller, more focused modules?**
  _Cohesion score 0.08258258258258258 - nodes in this community are weakly interconnected._
- **Should `Server & Bench Wiring` be split into smaller, more focused modules?**
  _Cohesion score 0.09401709401709402 - nodes in this community are weakly interconnected._