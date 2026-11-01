#pragma once

// Internal interfaces of netcfg (not part of the public API).

#include "cJSON.h"
#include "netcfg_config.h"

/** The JSON object of a configuration (the shape of netcfg_to_json), for embedding in other documents. NULL when out of memory. */
cJSON *netcfg_config_to_cjson(const netcfg_config_t *cfg);
