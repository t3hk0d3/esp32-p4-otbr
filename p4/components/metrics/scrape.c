// metrics_write: copies each source once (none of them takes the OpenThread lock), then renders without holding a
// lock and hands the body out in chunks of at most METRICS_CHUNK_BYTES.
#include <stdlib.h>

#include "collector.h"
#include "metrics.h"
#include "render.h"
#include "routing.h"

#define METRICS_CHUNK_BYTES 2048

// Everything a scrape copies, in one allocation.
typedef struct {
    char chunk[METRICS_CHUNK_BYTES];
    diag_metrics_view_t diag;
    metrics_collector_view_t collector;
    routing_metrics_view_t routing;
} scrape_t;

esp_err_t metrics_write(const metrics_device_t *device, metrics_flush_fn flush, void *ctx)
{
    scrape_t *s = malloc(sizeof(*s));

    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    const metrics_input_t in = {
        .device = device,
        .diag = diag_metrics_view(&s->diag) == ESP_OK ? &s->diag : NULL,
        .collector = metrics_collector_copy(&s->collector) == ESP_OK ? &s->collector : NULL,
        .routing = routing_metrics_view(&s->routing) == ESP_OK ? &s->routing : NULL,
    };
    metrics_writer_t w;

    metrics_writer_init(&w, s->chunk, sizeof(s->chunk), flush, ctx);
    metrics_render(&w, &in);
    const esp_err_t err = metrics_writer_finish(&w);

    free(s);
    return err;
}
