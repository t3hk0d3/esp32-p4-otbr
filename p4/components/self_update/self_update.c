#include "self_update.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "self_update_priv.h"

#define BASE_MAX 128
#define URL_MAX 192
#define DETAIL_MAX 160

static SemaphoreHandle_t s_mutex;
static self_update_snapshot_t s_cache;
static int64_t s_checked_at_us;
static char s_base[BASE_MAX] = SELF_UPDATE_RELEASES_URL;
static self_update_ops_t s_ops; // empty: the target default applies; host tests set fakes

// Created before any task runs, so the lock never needs a lazy, racy init.
__attribute__((constructor)) static void mutex_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
}

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static const self_update_ops_t *ops(void)
{
#ifndef CONFIG_IDF_TARGET_LINUX
    if (!s_ops.fetch) {
        return &self_update_http_ops;
    }
#endif
    return &s_ops;
}

void self_update_set_ops(const self_update_ops_t *new_ops)
{
    lock();
    s_ops = *new_ops;
    unlock();
}

void self_update_reset(void)
{
    lock();
    memset(&s_cache, 0, sizeof(s_cache));
    s_checked_at_us = 0;
    snprintf(s_base, sizeof(s_base), "%s", SELF_UPDATE_RELEASES_URL);
    unlock();
}

const char *self_update_base(void)
{
    return s_base;
}

#ifdef OTBR_TEST_BUILD
void self_update_test_set_base(const char *base)
{
    lock();
    snprintf(s_base, sizeof(s_base), "%s", (base && base[0]) ? base : SELF_UPDATE_RELEASES_URL);
    unlock();
}
#endif

// Fetch and parse; no lock is held during the network call. `detail` is always a valid string.
static esp_err_t fetch_and_parse(self_update_release_t *out, char *detail, size_t cap)
{
    char url[URL_MAX];
    char base[BASE_MAX];
    char buf[SELF_UPDATE_VERSIONS_MAX + 1];
    size_t len = 0;

    if (cap > 0) {
        detail[0] = '\0';
    }
    lock();
    snprintf(base, sizeof(base), "%s", s_base);
    const self_update_ops_t fetch_ops = *ops();
    unlock();

    esp_err_t err = self_update_asset_url(base, "", "versions.txt", url, sizeof(url));

    if (err != ESP_OK) {
        snprintf(detail, cap, "versions.txt URL too long");
        return err;
    }
    err = fetch_ops.fetch(url, buf, sizeof(buf), &len, detail, cap);
    if (err != ESP_OK) {
        return err;
    }
    err = self_update_parse_versions(buf, len, out);
    if (err != ESP_OK) {
        snprintf(detail, cap, "versions.txt not valid (%s): %s", url, esp_err_to_name(err));
    }
    return err;
}

esp_err_t self_update_fetch_release(self_update_release_t *out, char *detail, size_t cap)
{
    char local[DETAIL_MAX];
    char *dest = detail ? detail : local;
    const size_t dest_cap = detail ? cap : sizeof(local);

    return fetch_and_parse(out, dest, dest_cap);
}

bool self_update_check_begin(void)
{
    lock();
    const bool idle = s_cache.state != SELF_UPDATE_CHECKING;

    if (idle) {
        s_cache.state = SELF_UPDATE_CHECKING;
    }
    unlock();
    return idle;
}

void self_update_check_run(void)
{
    self_update_release_t release;
    char detail[DETAIL_MAX];
    const esp_err_t err = fetch_and_parse(&release, detail, sizeof(detail));

    lock();
    const int64_t now = ops()->now_us();

    s_checked_at_us = now;
    s_cache.has_age = true;
    if (err == ESP_OK) {
        s_cache.state = SELF_UPDATE_DONE;
        s_cache.error[0] = '\0';
        s_cache.release = release;
        s_cache.has_release = true;
    } else {
        s_cache.state = SELF_UPDATE_FAILED;
        snprintf(s_cache.error, sizeof(s_cache.error), "%s", detail);
    }
    unlock();
}

void self_update_get(self_update_snapshot_t *out)
{
    lock();
    *out = s_cache;
    if (out->has_age) {
        const int64_t elapsed = ops()->now_us() - s_checked_at_us;

        out->age_s = elapsed > 0 ? (uint32_t)(elapsed / 1000000) : 0;
    }
    unlock();
}
