#pragma once

// Test sample: the sketch mesh of the spec. This board is router 34 (0x8800), the leader is router 9 (0x2400).
// Routers 3 (two children) and 17 (one child, children_detail false) answered, router 50 answered without children,
// router 25 is a stub. Nine children in all; three SRP hosts (one from this board, one named a"b\<c>), three SRP servers; both OMR prefixes.

#include "scan_types.h"

/** A fresh copy of the sample, so a test may change it. Valid until the next call. */
mesh_scan_result_t *sample_result(void);
