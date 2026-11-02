#include <string.h>
#include "base_image.h"

#define VERSION_MAX 32 /* esp_app_desc_t.version */

static rcp_sync_base_image_t s_image;
static bool s_set;
static char s_version[VERSION_MAX];

static bool range_ok(const uint8_t *start, const uint8_t *end)
{
    return start && end && end > start;
}

esp_err_t rcp_sync_set_base_image(const rcp_sync_base_image_t *image)
{
    if (!image || !range_ok(image->bootloader_start, image->bootloader_end) || !range_ok(image->ptable_start, image->ptable_end) ||
            !range_ok(image->updater_start, image->updater_end) || !range_ok(image->version_start, image->version_end)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = (size_t)(image->version_end - image->version_start);

    s_image = *image;
    if (len >= sizeof(s_version)) {
        len = sizeof(s_version) - 1;
    }
    memcpy(s_version, image->version_start, len);
    while (len > 0 && (s_version[len - 1] == '\n' || s_version[len - 1] == '\r' || s_version[len - 1] == ' ')) {
        len--;
    }
    s_version[len] = '\0';
    s_set = true;
    return ESP_OK;
}

const rcp_sync_base_image_t *rcp_sync_base_image(void)
{
    return s_set ? &s_image : NULL;
}

const char *rcp_sync_base_image_version(void)
{
    return s_version;
}
