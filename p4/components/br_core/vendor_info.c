#include "vendor_info.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#define STAMP_LEN 16 // YYYYMMDDTHHMMSSZ
#define DATE_LEN 8
#define HASH_LEN 7

static bool digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static bool stamped(const char *v)
{
    if (!digits(v, DATE_LEN) || v[DATE_LEN] != 'T' || !digits(v + DATE_LEN + 1, 6) || v[STAMP_LEN - 1] != 'Z' ||
            v[STAMP_LEN] != '-') {
        return false;
    }
    const char *hash = v + STAMP_LEN + 1;
    for (size_t i = 0; i < HASH_LEN; i++) {
        if (!isxdigit((unsigned char)hash[i])) {
            return false;
        }
    }
    const char *rest = hash + HASH_LEN;
    return *rest == '\0' || strcmp(rest, "-dirty") == 0;
}

void br_core_vendor_sw_version(const char *project_ver, char out[BR_CORE_VENDOR_SW_VERSION_SIZE])
{
    if (!project_ver) {
        out[0] = '\0';
        return;
    }
    if (strlen(project_ver) >= STAMP_LEN + 1 + HASH_LEN && stamped(project_ver)) {
        memcpy(out, project_ver, DATE_LEN);
        out[DATE_LEN] = '-';
        memcpy(out + DATE_LEN + 1, project_ver + STAMP_LEN + 1, HASH_LEN);
        out[DATE_LEN + 1 + HASH_LEN] = '\0';
        return;
    }
    size_t n = strnlen(project_ver, BR_CORE_VENDOR_SW_VERSION_SIZE - 1);
    memcpy(out, project_ver, n);
    out[n] = '\0';
}
