#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "mux_gone_answer.h"
#include "spinel.h"

TEST_CASE("On a SET of STREAM_RAW (a frame to send): the transmit-done ABORT answer", "[ot_port]")
{
    TEST_ASSERT_EQUAL(OT_MUX_GONE_TX_ABORT, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_STREAM_RAW));
}

TEST_CASE("On a SET, INSERT or REMOVE of any other property: LAST_STATUS OK", "[ot_port]")
{
    TEST_ASSERT_EQUAL(OT_MUX_GONE_OK, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID));
    TEST_ASSERT_EQUAL(OT_MUX_GONE_OK, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_ENABLED));
    TEST_ASSERT_EQUAL(OT_MUX_GONE_OK, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_RCP_MAC_KEY));
    TEST_ASSERT_EQUAL(OT_MUX_GONE_OK, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_INSERT,
                                                         SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
    TEST_ASSERT_EQUAL(OT_MUX_GONE_OK, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_REMOVE,
                                                         SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES));
}

TEST_CASE("On a GET whose caller takes an error (RSSI, TX power, CSL accuracy, time): LAST_STATUS FAILURE",
          "[ot_port]")
{
    const uint32_t keys[] = {SPINEL_PROP_PHY_RSSI, SPINEL_PROP_PHY_TX_POWER, SPINEL_PROP_PHY_CCA_THRESHOLD,
                             SPINEL_PROP_RCP_CSL_ACCURACY, SPINEL_PROP_RCP_TIMESTAMP, SPINEL_PROP_PHY_RX_SENSITIVITY
                            };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        TEST_ASSERT_EQUAL(OT_MUX_GONE_FAILURE, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_GET, keys[i]));
    }
}

TEST_CASE("On a GET of the supported or preferred channel mask: the 2.4 GHz channels answer", "[ot_port]")
{
    TEST_ASSERT_EQUAL(OT_MUX_GONE_CHANNELS, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_GET,
                                                               SPINEL_PROP_PHY_CHAN_SUPPORTED));
    TEST_ASSERT_EQUAL(OT_MUX_GONE_CHANNELS, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_GET,
                                                               SPINEL_PROP_PHY_CHAN_PREFERRED));
}

TEST_CASE("On a GET read only at OpenThread's init that ends in SuccessOrDie (HWADDR, caps, versions): no answer",
          "[ot_port]")
{
    const uint32_t keys[] = {SPINEL_PROP_HWADDR, SPINEL_PROP_RADIO_CAPS, SPINEL_PROP_PROTOCOL_VERSION,
                             SPINEL_PROP_NCP_VERSION
                            };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        TEST_ASSERT_EQUAL(OT_MUX_GONE_WAIT, ot_mux_gone_answer(SPINEL_CMD_PROP_VALUE_GET, keys[i]));
    }
}

TEST_CASE("On a RESET command: no answer", "[ot_port]")
{
    TEST_ASSERT_EQUAL(OT_MUX_GONE_WAIT, ot_mux_gone_answer(SPINEL_CMD_RESET, 0));
}

TEST_CASE("On an outgoing SET of STREAM_RAW: TID, command and the two-byte property", "[ot_port]")
{
    const uint8_t f[] = {0x83, SPINEL_CMD_PROP_VALUE_SET, 0x71, 0x05, 0x00};
    uint8_t tid;
    uint32_t cmd, key;

    TEST_ASSERT_EQUAL(0x71, SPINEL_PROP_STREAM_RAW);
    TEST_ASSERT_TRUE(ot_mux_gone_parse(f, sizeof(f), &tid, &cmd, &key));
    TEST_ASSERT_EQUAL(3, tid);
    TEST_ASSERT_EQUAL(SPINEL_CMD_PROP_VALUE_SET, cmd);
    TEST_ASSERT_EQUAL(SPINEL_PROP_STREAM_RAW, key);
}

TEST_CASE("On a GET of a property above 127: the packed property", "[ot_port]")
{
    const uint8_t f[] = {0x81, SPINEL_CMD_PROP_VALUE_GET, 0x8B, 0x24}; /* RADIO_CAPS 0x120B */
    uint8_t tid;
    uint32_t cmd, key;

    TEST_ASSERT_TRUE(ot_mux_gone_parse(f, sizeof(f), &tid, &cmd, &key));
    TEST_ASSERT_EQUAL(SPINEL_PROP_RADIO_CAPS, key);
}

TEST_CASE("On a frame without the header flag or too short: not parsed", "[ot_port]")
{
    const uint8_t no_flag[] = {0x01, 0x02, 0x03};
    const uint8_t short_frame[] = {0x81, 0x02};
    uint8_t tid;
    uint32_t cmd, key;

    TEST_ASSERT_FALSE(ot_mux_gone_parse(no_flag, sizeof(no_flag), &tid, &cmd, &key));
    TEST_ASSERT_FALSE(ot_mux_gone_parse(short_frame, sizeof(short_frame), &tid, &cmd, &key));
}

TEST_CASE("On an OK answer: PROP_VALUE_IS LAST_STATUS OK with the TID", "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    const uint8_t want[] = {0x85, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS, SPINEL_STATUS_OK};

    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_frame(OT_MUX_GONE_OK, 5, SPINEL_PROP_MAC_15_4_PANID, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a FAILURE answer: LAST_STATUS FAILURE", "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    const uint8_t want[] = {0x82, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS, SPINEL_STATUS_FAILURE};

    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_frame(OT_MUX_GONE_FAILURE, 2, SPINEL_PROP_PHY_RSSI, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a TX_ABORT answer: stack-native ABORT (16395 packed) then frame pending and header updated false",
          "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    const uint8_t want[] = {0x8F, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS, 0x8B, 0x80, 0x01, 0x00, 0x00};

    TEST_ASSERT_EQUAL(16395, SPINEL_STATUS_STACK_NATIVE__BEGIN + 11);
    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_frame(OT_MUX_GONE_TX_ABORT, 15, SPINEL_PROP_STREAM_RAW, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a CHANNELS answer for the supported mask: PROP_VALUE_IS PHY_CHAN_SUPPORTED with channels 11 to 26",
          "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    uint8_t want[3 + 16] = {0x84, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_PHY_CHAN_SUPPORTED};

    for (uint8_t c = 11; c <= 26; c++) {
        want[3 + c - 11] = c;
    }
    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_frame(OT_MUX_GONE_CHANNELS, 4, SPINEL_PROP_PHY_CHAN_SUPPORTED, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a CHANNELS answer for the preferred mask: PROP_VALUE_IS PHY_CHAN_PREFERRED with channels 11 to 26",
          "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    uint8_t want[3 + 16] = {0x86, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_PHY_CHAN_PREFERRED};

    for (uint8_t c = 11; c <= 26; c++) {
        want[3 + c - 11] = c;
    }
    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_frame(OT_MUX_GONE_CHANNELS, 6, SPINEL_PROP_PHY_CHAN_PREFERRED, f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a CHANNELS answer about a property above 127: the property packed in two bytes", "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];

    TEST_ASSERT_EQUAL(4 + 16, ot_mux_gone_frame(OT_MUX_GONE_CHANNELS, 1, SPINEL_PROP_RADIO_CAPS, f));
    TEST_ASSERT_EQUAL_HEX8(0x8B, f[2]);
    TEST_ASSERT_EQUAL_HEX8(0x24, f[3]);
    TEST_ASSERT_EQUAL(11, f[4]);
    TEST_ASSERT_EQUAL(26, f[19]);
}

TEST_CASE("On an outgoing RESET command: recognised as a reset", "[ot_port]")
{
    const uint8_t f[] = {SPINEL_HEADER_FLAG, SPINEL_CMD_RESET, SPINEL_RESET_STACK};
    const uint8_t bare[] = {SPINEL_HEADER_FLAG, SPINEL_CMD_RESET};

    TEST_ASSERT_TRUE(ot_mux_gone_is_reset(f, sizeof(f)));
    TEST_ASSERT_TRUE(ot_mux_gone_is_reset(bare, sizeof(bare)));
}

TEST_CASE("On any other outgoing frame: not a reset", "[ot_port]")
{
    const uint8_t get[] = {0x81, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_HWADDR};
    const uint8_t no_flag[] = {0x00, SPINEL_CMD_RESET, SPINEL_RESET_STACK};
    const uint8_t short_frame[] = {SPINEL_HEADER_FLAG};

    TEST_ASSERT_FALSE(ot_mux_gone_is_reset(get, sizeof(get)));
    TEST_ASSERT_FALSE(ot_mux_gone_is_reset(no_flag, sizeof(no_flag)));
    TEST_ASSERT_FALSE(ot_mux_gone_is_reset(short_frame, sizeof(short_frame)));
    TEST_ASSERT_FALSE(ot_mux_gone_is_reset(NULL, 3));
}

TEST_CASE("On the reset notification after GONE: LAST_STATUS RESET_UNKNOWN with TID 0", "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];
    const uint8_t want[] = {SPINEL_HEADER_FLAG, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS,
                            SPINEL_STATUS_RESET_UNKNOWN
                           };

    TEST_ASSERT_EQUAL(sizeof(want), ot_mux_gone_reset_frame(f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On WAIT: no frame", "[ot_port]")
{
    uint8_t f[OT_MUX_GONE_FRAME_MAX];

    TEST_ASSERT_EQUAL(0, ot_mux_gone_frame(OT_MUX_GONE_WAIT, 1, SPINEL_PROP_HWADDR, f));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
