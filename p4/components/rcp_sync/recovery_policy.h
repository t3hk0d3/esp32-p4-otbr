#pragma once

// When the probe falls back to ROM recovery (spec 5.3 step 3.2). Pure.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool jumper_present;
    bool updater_reachable;     /**< the updater answered HELLO */
    const char *updater_version; /**< from HELLO, "" when not reachable */
    const char *base_version;   /**< version of the embedded base image; NULL or "" = none embedded */
    bool already_attempted;     /**< an automatic recovery already ran in this boot */
} rcp_sync_recovery_input_t;

/**
 * True when the base image must be rewritten: the jumper is present, a base image is embedded, no automatic recovery
 * ran yet this boot, and the updater is silent or its version is not the embedded one (string equality).
 */
bool rcp_sync_recovery_wanted(const rcp_sync_recovery_input_t *in);

#ifdef __cplusplus
}
#endif
