# Gemini & Antigravity Project Instructions

Refer to [AGENTS.md](file:///d:/GitHub/distributed-rate-limiter/AGENTS.md) for full project rules, coding standards, and skill guidelines adopting the [VoltAgent/awesome-agent-skills](https://github.com/VoltAgent/awesome-agent-skills) specification.

### Key Tenets:
- **Core Interface Decoupling**: Keep `ratelimiter::RateLimiterCore` isolated from gRPC dependencies.
- **Skill Quality Rules**: All agent skills in `.agents/skills/` must follow third-person descriptions, progressive disclosure (<100 token header, <500 lines body), portable relative paths, and scoped tools.
- **Resilience**: Ensure graceful draining on server shutdown, deadlines on gRPC clients, and thread-safe quota evaluation.
