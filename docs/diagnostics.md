# Diagnostics

`GET /device/diag` returns the precomputed snapshot of a sampler that runs once per second: per-core load over 1/10/60 s,
per-task CPU share, core, priority, minimum free stack, internal RAM and PSRAM figures, OpenThread lock statistics (count,
longest hold and wait, holder), uptime, last reset reason and the chip temperature. The Diagnostics page (`/diagnostics.html`) shows all of it except the chip temperature, which is on the Status page, refreshed every 2 s.

<img src="images/diagnostics.png" width="640" alt="Diagnostics page: CPU load per core over 1, 10 and 60 s, internal RAM and PSRAM, OpenThread lock statistics and the task table">

- The lock statistics cover everything that takes the OpenThread lock through `esp_openthread_lock_acquire/release`
  (libopenthread, `br_core`, the Thread REST server). The border router internals in the prebuilt `libopenthread_br.a`
  only use the task-switching lock, which is not counted: their holds show up only as wait time of the others.
- The chip temperature (`chip_temperature_c`, one decimal, also on the Status page and as `br_chip_temperature_celsius`) is the die temperature, not the room temperature: it reads above ambient and rises with load. The driver picks the most accurate range for the reading (about ±1 °C up to 80 °C, ±2 °C above), and the value moves in 1 °C steps on a given board even though it is shown with one decimal. It is `null` (shown as `-`) when the sensor did not start or the last read failed.
- Diagnostics are optional: if the sampler cannot start, the boot goes on and `/device/diag` answers 503 `not_started`.
- Tasks beyond the 48 the sampler tracks are left out of the snapshot (one log line).
- Tasks are pinned: core 0 for the network path (lwIP, Ethernet, OpenThread, mDNS), core 1 for management (HTTP, workers,
  `rcp_sync`, console).
