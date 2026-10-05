# Agent Guidelines & Quality Standards

This project adopts the Agent Skills specification and quality standards established by [VoltAgent/awesome-agent-skills](https://github.com/VoltAgent/awesome-agent-skills).

All agents operating in this repository (Antigravity, Gemini CLI, Claude Code, Cursor, Codex, etc.) must adhere to the rules and standards detailed below.

---

## 1. Skill Quality Standards (from VoltAgent/awesome-agent-skills)

Every skill in `.agents/skills/` must strictly satisfy these four core criteria:

| Quality Area | Mandated Rule |
| :--- | :--- |
| **Description** | Write in third person. Clearly declare *what* the skill does and *when* to invoke it. Include precise matching keywords (e.g., "Token Bucket algorithm", "gRPC timeout", "memory safety"). |
| **Progressive Disclosure** | Keep top-level frontmatter metadata under 100 tokens. The skill body (`SKILL.md`) must not exceed 500 lines. Heavy references, schemas, and benchmarks must be placed in subdirectories (e.g., `references/`, `examples/`) and loaded on demand. |
| **No Absolute Paths** | Never hard-code machine-specific paths (e.g., `/home/user/` or `C:\Users\...`). Always use relative paths or portable variables (`$PROJECT_ROOT`). |
| **Scoped Tools** | Explicitly declare tool dependencies. Request only the tools the skill actually requires; never use blanket wildcards (`tools: ["*"]`). |

---

## 2. Directory Layout for Agent Skills

Per the cross-agent compatibility specification in `awesome-agent-skills`:

- **Project Path**: `.agents/skills/<skill-name>/SKILL.md`
- **Optional Auxiliary Files**:
  - `.agents/skills/<skill-name>/references/` (documentation loaded on demand)
  - `.agents/skills/<skill-name>/examples/` (reference implementations)
  - `.agents/skills/<skill-name>/scripts/` (reusable automation utilities)

---

## 3. Project Engineering Rules (Distributed Rate Limiter)

When developing or refactoring this C++ & gRPC distributed rate limiter:

1. **Strict Layer Decoupling**:
   - The core rate limiting algorithms must live in `include/` and `src/` as implementations of `ratelimiter::RateLimiterCore`.
   - Core algorithms must **never** include `<grpcpp/grpcpp.h>` or generated protobuf headers.
   - The gRPC transport layer (`RateLimiterServiceImpl`) depends on `RateLimiterCore` via dependency injection (`std::shared_ptr<RateLimiterCore>`).
2. **Modern C++ Standards**:
   - Use C++17 or C++20 features (e.g., `std::string_view`, `std::chrono`, `std::unique_ptr`, `std::shared_ptr`, `std::atomic`).
   - Explicitly avoid raw pointers for ownership transfer.
   - Enforce RAII for locks and resources (`std::lock_guard`, `std::unique_lock`, `std::scoped_lock`).
3. **Distributed Systems & Production Resilience**:
   - All gRPC clients must set explicit RPC deadlines (`context.set_deadline(...)`).
   - The server must handle graceful shutdown (`server->Shutdown(deadline)`) upon receiving `SIGINT` or `SIGTERM`, draining in-flight requests.
   - Handle client cancellation cleanly via `context->IsCancelled()`.
4. **Thread Safety & Performance**:
   - Rate limiter algorithms must be thread-safe for high-concurrency environments.
   - Prefer fine-grained locking (per-key/client mutex) or lock-free atomic counters over global coarse-grained locks.
