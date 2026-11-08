#include <string.h>
#include "unity.h"
#include "otbr_probe.h"

TEST_CASE("On the reset frame: CMD_RESET of the stack with TID 0", "[otbr_probe]")
{
    uint8_t f[OTBR_PROBE_FRAME_MAX];
    const uint8_t want[] = {0x80, 0x01, 0x02};

    TEST_ASSERT_EQUAL(3, otbr_probe_reset_frame(f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, 3);
}

TEST_CASE("On a GET of RADIO_CAPS: the property packed in two bytes", "[otbr_probe]")
{
    uint8_t f[OTBR_PROBE_FRAME_MAX];
    const uint8_t want[] = {0x83, 0x02, 0x8B, 0x24};

    TEST_ASSERT_EQUAL(4, otbr_probe_get_frame(3, OTBR_SPINEL_PROP_RADIO_CAPS, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, 4);
}

TEST_CASE("On a GET with TID 0 or above 15: no frame", "[otbr_probe]")
{
    uint8_t f[OTBR_PROBE_FRAME_MAX];

    TEST_ASSERT_EQUAL(0, otbr_probe_get_frame(0, OTBR_SPINEL_PROP_NCP_VERSION, f));
    TEST_ASSERT_EQUAL(0, otbr_probe_get_frame(16, OTBR_SPINEL_PROP_NCP_VERSION, f));
}

TEST_CASE("On a reset notification (LAST_STATUS RESET_SOFTWARE, TID 0): parsed and seen as a reset", "[otbr_probe]")
{
    const uint8_t f[] = {0x80, 0x06, 0x00, 0x72};
    otbr_spinel_msg_t m;

    TEST_ASSERT_TRUE(otbr_probe_parse(f, sizeof(f), &m));
    TEST_ASSERT_TRUE(otbr_probe_is_reset(&m));
}

TEST_CASE("On LAST_STATUS FAILURE or a reset status with a TID: not a reset", "[otbr_probe]")
{
    const uint8_t failure[] = {0x80, 0x06, 0x00, 0x01};
    const uint8_t with_tid[] = {0x81, 0x06, 0x00, 0x72};
    otbr_spinel_msg_t m;

    TEST_ASSERT_TRUE(otbr_probe_parse(failure, sizeof(failure), &m));
    TEST_ASSERT_FALSE(otbr_probe_is_reset(&m));
    TEST_ASSERT_TRUE(otbr_probe_parse(with_tid, sizeof(with_tid), &m));
    TEST_ASSERT_FALSE(otbr_probe_is_reset(&m));
}

TEST_CASE("On an NCP_VERSION answer: the TID, key and the version text", "[otbr_probe]")
{
    const uint8_t f[] = {0x81, 0x06, 0x02, 'O', 'P', 'E', 'N', 0};
    otbr_spinel_msg_t m;

    TEST_ASSERT_TRUE(otbr_probe_parse(f, sizeof(f), &m));
    TEST_ASSERT_TRUE(otbr_probe_is_answer(&m, 1, OTBR_SPINEL_PROP_NCP_VERSION));
    TEST_ASSERT_FALSE(otbr_probe_is_answer(&m, 2, OTBR_SPINEL_PROP_NCP_VERSION));
    TEST_ASSERT_EQUAL_STRING("OPEN", (const char *)m.value);
}

TEST_CASE("On bytes without the spinel header flag or too short: not parsed", "[otbr_probe]")
{
    const uint8_t bad_flag[] = {0x01, 0x06, 0x02};
    const uint8_t short_frame[] = {0x80, 0x06};
    otbr_spinel_msg_t m;

    TEST_ASSERT_FALSE(otbr_probe_parse(bad_flag, sizeof(bad_flag), &m));
    TEST_ASSERT_FALSE(otbr_probe_parse(short_frame, sizeof(short_frame), &m));
}

TEST_CASE("On a CAPS list with CONFIG_RADIO among packed values: found; without it: not", "[otbr_probe]")
{
    const uint8_t rcp[] = {0x05, 0x22, 0x81, 0x40}; /* 5, 34, 8193 */
    const uint8_t ncp[] = {0x05, 0x21};

    TEST_ASSERT_TRUE(otbr_probe_has_cap(rcp, sizeof(rcp), OTBR_SPINEL_CAP_CONFIG_RADIO));
    TEST_ASSERT_FALSE(otbr_probe_has_cap(ncp, sizeof(ncp), OTBR_SPINEL_CAP_CONFIG_RADIO));
}

TEST_CASE("On the CC2652P's radio caps 0x7f: nothing missing", "[otbr_probe]")
{
    const uint8_t v[] = {0x7F};
    uint32_t caps = 0;
    char text[96];

    TEST_ASSERT_TRUE(otbr_probe_radio_caps(v, sizeof(v), &caps));
    TEST_ASSERT_EQUAL_HEX32(0x7F, caps);
    TEST_ASSERT_EQUAL_HEX32(0, otbr_probe_missing_caps(caps, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("", text);
}

TEST_CASE("On radio caps without tx-security and tx-timing: both named as missing", "[otbr_probe]")
{
    char text[96];
    const uint32_t missing = otbr_probe_missing_caps(OTBR_RADIO_CAP_ACK_TIMEOUT | OTBR_RADIO_CAP_TRANSMIT_RETRIES |
                                                     OTBR_RADIO_CAP_CSMA_BACKOFF, text, sizeof(text));

    TEST_ASSERT_EQUAL_HEX32(OTBR_RADIO_CAP_TRANSMIT_SEC | OTBR_RADIO_CAP_TRANSMIT_TIMING, missing);
    TEST_ASSERT_EQUAL_STRING("missing radio caps: tx-security, tx-timing", text);
}
