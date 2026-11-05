#include <string.h>
#include "device_api_priv.h"

int device_api_length_check(size_t len, size_t max)
{
    if (len == 0) {
        return 400;
    }
    return len > max ? 413 : 0;
}

device_api_error_t device_api_check_url(const char *url)
{
    static const device_api_error_t invalid = {400, "invalid_request", "the URL must start with http:// or https://"};
    static const device_api_error_t ok = {0, NULL, NULL};

    if (url == NULL) {
        return invalid;
    }
    const char *host = strncmp(url, "https://", 8) == 0 ? url + 8 : strncmp(url, "http://", 7) == 0 ? url + 7 : NULL;

    if (host == NULL || host[0] == '\0' || strchr("/?#", host[0]) != NULL) {
        return invalid;
    }
    return ok;
}

int device_api_installed_json(char *buf, size_t cap, bool known, const char *version)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"installed\":");
    json_out_str(&w, known && version && version[0] ? version : NULL);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}
