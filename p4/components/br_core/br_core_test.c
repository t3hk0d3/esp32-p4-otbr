// Test-only hooks of br_core (compiled only when OTBR_TEST_BUILD is defined).
#ifdef OTBR_TEST_BUILD

#include <string.h>

#include "br_core.h"
#include "br_core_priv.h"
#include "freertos/FreeRTOS.h"

// ot_start_abandon: the channel whose next OpenThread start ends as an abandoned one ("" for none). Not persisted.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_abandon_id[BR_CORE_CHANNEL_ID_MAX];

void br_core_test_set_start_abandon(const char *id)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_abandon_id, id ? id : "", sizeof(s_abandon_id));
    taskEXIT_CRITICAL(&s_lock);
}

bool core_test_take_start_abandon(const char *id)
{
    taskENTER_CRITICAL(&s_lock);
    const bool take = s_abandon_id[0] != '\0' && strcmp(s_abandon_id, id) == 0;

    if (take) {
        s_abandon_id[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_lock);
    return take;
}

#endif // OTBR_TEST_BUILD
