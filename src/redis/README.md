# redis/ (Weeks 1-3)

`redis_backend.h` is the abstraction the engines depend on. The concrete client (decided in the
Day 3 spike: redis-plus-plus async cluster, fallback hiredis + libuv) must support pipelining,
async I/O, connection pooling and Redis Cluster. Scripts are loaded with SCRIPT LOAD at startup,
called with EVALSHA, and retried on NOSCRIPT.
