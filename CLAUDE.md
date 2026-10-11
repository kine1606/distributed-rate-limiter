# Claude Code project instructions

Read `AGENTS.md` for the engineering rules (layer decoupling, modern C++, resilience, thread safety)
and `docs/ROADMAP.md` for the week-by-week checklist. Each module folder has a `README.md` with its skeleton, build order and hints; read it before changing that module. The spec summary is in `README.md`.

## Subagents (`.agents/agents/`)

Claude Code loads subagents from `.claude/agents/`. Copy them there once: `mkdir -p .claude/agents && cp .agents/agents/*.md .claude/agents/` (NOTICE.md is attribution only).

- `cpp-pro`: use for C++ implementation and refactoring (lease map, Redis client wrapper, server).
- `performance-engineer`: use for profiling, load tests, and the benchmark report.

## Commands

- Build and test: `scripts/test.sh release`, `scripts/test.sh tsan`, `scripts/test.sh asan`
- Run: `./build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml`

## Rules that matter most

- Do not optimize without a benchmark or profile number; report before/after.
- No per-request logging, mutex or heap allocation on the lease hot path.
- Never report an extrapolated throughput as a measured one.
