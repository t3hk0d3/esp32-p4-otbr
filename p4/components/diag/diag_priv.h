#pragma once

// Internal glue between the target-only files of diag.

#include "lock_stats.h"

/** Copy of the OpenThread lock figures (taken under the wrapper's spinlock). */
void diag_ot_lock_view(diag_lock_view_t *out);
