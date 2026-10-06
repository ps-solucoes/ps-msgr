# Backlog

Items found in the pre-1.0 review. Remove an item in the pull request that
resolves it.

## Async wait in the bindings

The C library has waitsets since 1.1 (`<psmsgr/waitset.h>`): one thread
waits for many readers. The bindings don't use them yet, so an async worker
still keeps a thread blocked in `Wait`. One small PR per binding:

- Python: an `asyncio` wait, completed with `call_soon_threadsafe`.
- Go: a wait that a `select` can use.

Each one falls back to a thread per reader where `psmsgr_waitset_open`
returns `NOTSUP`, adds `psmsgr_waitset_event` and `PSMSGR_WAITSET_MAX` to
`interop_helper layout`, and raises its minimum library minor to 1.
