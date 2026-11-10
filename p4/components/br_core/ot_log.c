// OpenThread's log lines, whole, through ESP-IDF's logger, and OpenThread's own level following the OPENTHREAD
// component level of the logging component.
//
// __wrap_otPlatLog replaces the platform's otPlatLog (linker --wrap, br_core CMakeLists.txt): ESP-IDF's version writes
// each line as three esp_log_write calls (prefix, body, newline), which the logging component's capture would see as
// three lines. Here the line is formatted once and written with one call, in the format of ESP_LOGx (no colours: the
// build has CONFIG_LOG_COLORS off), so logging_text_body strips it like any other line.

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "openthread/logging.h"
#include "logging.h"
#include "br_core_priv.h"
#include "ot_log_map.h"

#define OT_LOG_TAG "OPENTHREAD"
// On the stack of whichever task logs (the OpenThread task has 12 KB); a longer line is cut. Half of LOGGING_LINE_MAX.
#define OT_LINE_MAX 256
#define OT_CUT_MARK "[cut]"

static const char *TAG = "br_core";

static atomic_int s_level = ESP_LOG_INFO; // the OPENTHREAD component's effective level, as an esp_log_level_t

static char level_letter(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_ERROR:
        return 'E';
    case ESP_LOG_WARN:
        return 'W';
    case ESP_LOG_INFO:
        return 'I';
    default:
        return 'D';
    }
}

void __wrap_otPlatLog(otLogLevel log_level, otLogRegion log_region, const char *format, ...);

void __wrap_otPlatLog(otLogLevel log_level, otLogRegion log_region, const char *format, ...)
{
    (void)log_region;
    const esp_log_level_t level = br_core_ot_log_to_esp(log_level);

    if (level > esp_log_level_get(OT_LOG_TAG)) {
        return;
    }
    char text[OT_LINE_MAX];
    va_list args;

    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);

    va_end(args);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(text)) {
        memcpy(text + sizeof(text) - sizeof(OT_CUT_MARK), OT_CUT_MARK, sizeof(OT_CUT_MARK));
    }
    esp_log_write(level, OT_LOG_TAG, "%c (%lu) %s: %s\n", level_letter(level), (unsigned long)esp_log_timestamp(),
                  OT_LOG_TAG, text);
}

static void on_level(esp_log_level_t effective)
{
    atomic_store(&s_level, effective);
    core_ot_log_push(); // no-op while OpenThread is not up: br_core_ot_log_apply_locked applies the level at its start
}

void br_core_ot_log_apply_locked(void)
{
    otLoggingSetLevel(br_core_esp_to_ot_log((esp_log_level_t)atomic_load(&s_level)));
}

void br_core_ot_log_start(void)
{
    const esp_err_t err = logging_level_listener_add(OT_LOG_TAG, on_level);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OpenThread keeps its build log level: %s", esp_err_to_name(err));
    }
}
