// GET /metrics: copies each source once (none of them takes the OpenThread lock), then renders without holding a lock
// and streams the body in chunks of at most METRICS_CHUNK_BYTES.
#include <stdlib.h>

#include "br_core.h"
#include "collector.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "metrics.h"
#include "netcfg.h"
#include "render.h"
#include "sniffer.h"

#define METRICS_CHUNK_BYTES 2048
#define ETH_IFKEY "ETH_DEF" // used until netcfg has created the interfaces (as device_api)
#define CONTENT_TYPE "text/plain; version=0.0.4; charset=utf-8"

// Everything a scrape copies, in one allocation.
typedef struct {
    char chunk[METRICS_CHUNK_BYTES];
    br_core_status_t core;
    metrics_device_t device;
    diag_metrics_view_t diag;
    metrics_collector_view_t collector;
} scrape_t;

static esp_err_t send_chunk(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, (ssize_t)len);
}

static bool eth_up(void)
{
    esp_netif_t *netif = netcfg_mgmt_netif();

    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey(ETH_IFKEY);
    }
    return netif && esp_netif_is_netif_up(netif);
}

static void fill_device(scrape_t *s)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    sniffer_stats_t ss;

    br_core_get_status_brief(&s->core);
    sniffer_get_stats(&ss);
    s->device = (metrics_device_t) {
        .mode = s->core.mode,
        .version = esp_app_get_description()->version,
        .slot = running ? (int)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1,
        .rcp_version = s->core.rcp_installed_known ? s->core.rcp_installed : "",
        .rcp_compatible = s->core.rcp_compatible,
        .storage_ok = !s->core.storage_degraded,
        .sniffer_active = ss.active,
        .sniffer_frames = ss.frames,
        .sniffer_drops = ss.drops,
        .eth_up = eth_up(),
        .trial_pending = netcfg_trial_pending(),
    };
}

esp_err_t metrics_get_handler(httpd_req_t *req)
{
    scrape_t *s = malloc(sizeof(*s));

    if (!s) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"no_memory\",\"detail\":\"out of memory\"}");
    }
    fill_device(s);
    const metrics_input_t in = {
        .device = &s->device,
        .diag = diag_metrics_view(&s->diag) == ESP_OK ? &s->diag : NULL,
        .collector = metrics_collector_copy(&s->collector) == ESP_OK ? &s->collector : NULL,
    };
    metrics_writer_t w;

    httpd_resp_set_type(req, CONTENT_TYPE);
    metrics_writer_init(&w, s->chunk, sizeof(s->chunk), send_chunk, req);
    metrics_render(&w, &in);
    esp_err_t err = metrics_writer_finish(&w);

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(s);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}
