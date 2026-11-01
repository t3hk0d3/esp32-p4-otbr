#include "link.h"

esp_err_t rcp_sync_probe(rcp_sync_probe_result_t *out)
{
    return rcp_sync_probe_with(&rcp_sync_link_spi, out);
}

esp_err_t rcp_sync_push_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    return rcp_sync_push_firmware_with(&rcp_sync_link_spi, source, err_detail, err_cap);
}
