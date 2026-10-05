# reliability/ (Week 4)

Circuit breaker around Redis, timeouts (5-10 ms), bounded retries with jitter, backpressure
(bounded queues -> RESOURCE_EXHAUSTED), fail-open / fail-closed policy after lease exhaustion,
graceful shutdown (return unused leased tokens).
