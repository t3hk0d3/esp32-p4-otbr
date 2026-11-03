#pragma once

// Deferred reboots that wait for a running long operation (an update, a radio firmware push, a ROM recovery) instead
// of cutting it off. Every reboot that is not the long operation's own goes through reboot_gate_restart: the netcfg
// trial expiry and `net reset`, the Thread REST reset (DELETE /node). The long operations reboot by themselves.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers the "a long operation is running" predicate and creates the reboot timer (app_main, once, before anything
 * can reboot). Without a predicate nothing is ever deferred.
 */
void reboot_gate_set_busy(bool (*busy)(void));

/** True while the registered predicate says a long operation runs. */
bool reboot_gate_busy(void);

/**
 * Reboots after `delay_ms`, and then only once no long operation runs: while one does, the reboot is retried every
 * 2 s. Returns at once when the timer exists (safe from any task, also from an esp_timer callback); without it, it
 * waits in the caller.
 */
void reboot_gate_restart(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
