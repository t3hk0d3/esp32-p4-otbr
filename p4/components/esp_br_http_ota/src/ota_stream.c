#include "ota_stream.h"
#include <string.h>

static esp_err_t fail(ota_stream_t *stream, esp_err_t err)
{
    stream->state = OTA_STREAM_FAILED;
    return err;
}

esp_err_t ota_stream_start_check(bool boot_is_running, bool running_pending_verify)
{
    if (!boot_is_running) {
        return ESP_ERR_INVALID_STATE;
    }
    return running_pending_verify ? ESP_BR_OTA_ERR_NOT_CONFIRMED : ESP_OK;
}

void ota_stream_init(ota_stream_t *stream, const ota_stream_ops_t *ops, void *ctx)
{
    memset(stream, 0, sizeof(*stream));
    stream->ops = ops;
    stream->ctx = ctx;
    stream->state = OTA_STREAM_HEADER;
}

static esp_err_t consume_header(ota_stream_t *stream, const uint8_t **data, size_t *len)
{
    size_t take = OTA_BUNDLE_HEADER_LEN - stream->header_got;
    if (take > *len) {
        take = *len;
    }
    memcpy(stream->header_buf + stream->header_got, *data, take);
    stream->header_got += take;
    *data += take;
    *len -= take;
    if (stream->header_got < OTA_BUNDLE_HEADER_LEN) {
        return ESP_OK;
    }
    esp_err_t err = ota_bundle_parse_header(stream->header_buf, OTA_BUNDLE_HEADER_LEN, &stream->header);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    if (stream->header.web_len == 0) {
        return fail(stream, ESP_ERR_INVALID_SIZE);
    }
    err = stream->ops->web_begin(stream->ctx, stream->header.web_len);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    stream->state = OTA_STREAM_WEB;
    return ESP_OK;
}

static esp_err_t consume_web(ota_stream_t *stream, const uint8_t **data, size_t *len)
{
    size_t take = stream->header.web_len - stream->web_got;
    if (take > *len) {
        take = *len;
    }
    esp_err_t err = stream->ops->web_write(stream->ctx, stream->web_got, *data, take);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    stream->web_got += (uint32_t)take;
    *data += take;
    *len -= take;
    if (stream->web_got == stream->header.web_len) {
        err = stream->ops->web_verify(stream->ctx, stream->header.web_len, stream->header.web_sha256);
        if (err != ESP_OK) {
            return fail(stream, err);
        }
        stream->state = OTA_STREAM_APP;
    }
    return ESP_OK;
}

static esp_err_t consume_app(ota_stream_t *stream, const uint8_t **data, size_t *len)
{
    esp_err_t err = ESP_OK;
    if (!stream->app_open) {
        err = stream->ops->app_begin(stream->ctx);
        if (err != ESP_OK) {
            return fail(stream, err);
        }
        stream->app_open = true;
    }
    err = stream->ops->app_write(stream->ctx, *data, *len);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    stream->app_got += *len;
    *data += *len;
    *len = 0;
    return ESP_OK;
}

esp_err_t ota_stream_write(ota_stream_t *stream, const void *data, size_t len)
{
    if (!stream || (!data && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream->state == OTA_STREAM_FAILED || stream->state == OTA_STREAM_DONE) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = data;
    while (len > 0) {
        esp_err_t err = ESP_OK;
        switch (stream->state) {
        case OTA_STREAM_HEADER:
            err = consume_header(stream, &p, &len);
            break;
        case OTA_STREAM_WEB:
            err = consume_web(stream, &p, &len);
            break;
        default:
            err = consume_app(stream, &p, &len);
            break;
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t ota_stream_finish(ota_stream_t *stream)
{
    if (!stream) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream->state == OTA_STREAM_FAILED || stream->state == OTA_STREAM_DONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stream->state != OTA_STREAM_APP || stream->app_got == 0) {
        return fail(stream, ESP_ERR_INVALID_SIZE);
    }
    stream->app_open = false; /* app_end releases the session even when the validation fails */
    esp_err_t err = stream->ops->app_end(stream->ctx);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    err = stream->ops->commit(stream->ctx);
    if (err != ESP_OK) {
        return fail(stream, err);
    }
    stream->state = OTA_STREAM_DONE;
    return ESP_OK;
}

void ota_stream_abort(ota_stream_t *stream)
{
    if (!stream) {
        return;
    }
    if (stream->app_open) {
        stream->ops->app_abort(stream->ctx);
        stream->app_open = false;
    }
    if (stream->state != OTA_STREAM_DONE) {
        stream->state = OTA_STREAM_FAILED;
    }
}
