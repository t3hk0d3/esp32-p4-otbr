#pragma once

// The embedded C6 base image as given to rcp_sync_set_base_image (device build).

#include "rcp_sync.h"

/** NULL until rcp_sync_set_base_image ran. */
const rcp_sync_base_image_t *rcp_sync_base_image(void);

// rcp_sync_base_image_version: public, in rcp_sync.h.
