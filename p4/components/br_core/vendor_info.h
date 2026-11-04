#pragma once

// The software version this board reports in Network Diagnostic (Vendor SW Version TLV), derived from the build stamp
// (vendor_info.c). Pure, so it is host-tested on its own.

#ifdef __cplusplus
extern "C" {
#endif

#define BR_CORE_VENDOR_SW_VERSION_SIZE 17 /**< 16 characters (the TLV's limit) and a NUL */

/**
 * "YYYYMMDDTHHMMSSZ-<7 hex>[-dirty]" (PROJECT_VER) becomes "YYYYMMDD-<7 hex>". Anything else (also NULL) is cut to its
 * first 16 bytes. Always NUL-terminated; writes at most out[16].
 */
void br_core_vendor_sw_version(const char *project_ver, char out[BR_CORE_VENDOR_SW_VERSION_SIZE]);

#ifdef __cplusplus
}
#endif
