# Diagnostics

`GET /device/diag` returns the precomputed snapshot of a sampler that runs once per second: per-core load over 1/10/60 s,
per-task CPU share, core, priority, minimum free stack, internal RAM and PSRAM figures, OpenThread lock statistics (count,
longest hold and wait, holder), uptime and last reset reason. The Diagnostics page (`/diagnostics.html`) shows it refreshed every 2 s.

- The lock statistics cover everything that takes the OpenThread lock through `esp_openthread_lock_acquire/release`
  (libopenthread, `br_core`, the Thread REST server). The border router internals in the prebuilt `libopenthread_br.a`
  only use the task-switching lock, which is not counted: their holds show up only as wait time of the others.
- Diagnostics are optional: if the sampler cannot start, the boot goes on and `/device/diag` answers 503 `not_started`.
- Tasks beyond the 48 the sampler tracks are left out of the snapshot (one log line).
- Tasks are pinned: core 0 for the network path (lwIP, Ethernet, OpenThread, mDNS), core 1 for management (HTTP, workers,
  `rcp_sync`, console).
