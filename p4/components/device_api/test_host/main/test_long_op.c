#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"

static void release_all(void)
{
    device_api_op_end(DEVICE_API_OP_OTA);
    device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
    device_api_op_end(DEVICE_API_OP_ROM_RECOVER);
    device_api_op_end(DEVICE_API_OP_RADIO_MOVE);
    device_api_op_end(DEVICE_API_OP_PCAP);
}

TEST_CASE("On an OTA while nothing runs: it is granted", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    release_all();
}

TEST_CASE("On an OTA while an OTA runs: it is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    release_all();
}

TEST_CASE("On a firmware push while an OTA runs: it is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    release_all();
}

TEST_CASE("On an OTA while a firmware push runs: it is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    release_all();
}

TEST_CASE("On a release: the next operation is granted", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    release_all();
}

TEST_CASE("On releasing an operation that is not the running one: the running one stays", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    release_all();
}

TEST_CASE("On a pcap stream while an OTA or push runs: it is granted, a second pcap is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_PCAP));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_PCAP));
    release_all();
}

TEST_CASE("On a refused operation: the busy detail names what is running", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(DEVICE_API_OP_RADIO_PUSH), "update"));
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(DEVICE_API_OP_OTA), "firmware"));
    TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(DEVICE_API_OP_PCAP), "capture"));
    release_all();
}

TEST_CASE("On a finished OTA that was not released (committed, waits for the reboot): a later OTA or push is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA)); /* success path: no device_api_op_end */
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(DEVICE_API_OP_RADIO_PUSH), "reboot"));
    release_all();
}

TEST_CASE("On a long operation: device_api_op_long_running is true until it is released (reboot rule)", "[device_api]")
{
    release_all();
    TEST_ASSERT_FALSE(device_api_op_long_running());
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_TRUE(device_api_op_long_running());
    device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
    TEST_ASSERT_FALSE(device_api_op_long_running());
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_PCAP)); /* a capture is not a long operation */
    TEST_ASSERT_FALSE(device_api_op_long_running());
    release_all();
}

TEST_CASE("On a ROM recovery while an OTA, a push or a recovery runs: it is refused", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER));
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER));
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER));
    release_all();
}

TEST_CASE("On an OTA or a push while a ROM recovery runs: it is refused and the 409 detail names the recovery", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH));
    TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(DEVICE_API_OP_OTA), "ROM recovery"));
    TEST_ASSERT_TRUE(device_api_op_long_running());
    release_all();
    TEST_ASSERT_FALSE(device_api_op_long_running());
}

TEST_CASE("On a capture stream: device_api_op_pcap_running is true until it is released, and only for the pcap slot",
          "[device_api]")
{
    release_all();
    TEST_ASSERT_FALSE(device_api_op_pcap_running());
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA)); /* a long operation is not a capture */
    TEST_ASSERT_FALSE(device_api_op_pcap_running());
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_PCAP));
    TEST_ASSERT_TRUE(device_api_op_pcap_running());
    device_api_op_end(DEVICE_API_OP_PCAP);
    TEST_ASSERT_FALSE(device_api_op_pcap_running());
    release_all();
}

TEST_CASE("On a move of OTBR while an OTA, a push or a recovery runs: it is refused", "[device_api]")
{
    static const device_api_op_t others[] = {DEVICE_API_OP_OTA, DEVICE_API_OP_RADIO_PUSH, DEVICE_API_OP_ROM_RECOVER};

    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        release_all();
        TEST_ASSERT_TRUE(device_api_op_try_begin(others[i]));
        TEST_ASSERT_FALSE(device_api_op_try_begin(DEVICE_API_OP_RADIO_MOVE));
    }
    release_all();
}

TEST_CASE("On an OTA, a push, a recovery or another move while a move runs: refused, the 409 detail names the move",
          "[device_api]")
{
    static const device_api_op_t others[] = {DEVICE_API_OP_OTA, DEVICE_API_OP_RADIO_PUSH, DEVICE_API_OP_ROM_RECOVER,
                                             DEVICE_API_OP_RADIO_MOVE
                                            };

    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_MOVE));
    TEST_ASSERT_TRUE(device_api_op_long_running());
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        TEST_ASSERT_FALSE(device_api_op_try_begin(others[i]));
        TEST_ASSERT_NOT_NULL(strstr(device_api_op_busy_detail(others[i]), "moved"));
    }
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_PCAP)); /* a capture has its own slot */
    release_all();
}

TEST_CASE("On the end of a move: the slot is free again; an end by another holder leaves it", "[device_api]")
{
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_MOVE));
    device_api_op_end(DEVICE_API_OP_OTA);
    TEST_ASSERT_TRUE(device_api_op_long_running());
    device_api_op_end(DEVICE_API_OP_RADIO_MOVE);
    TEST_ASSERT_FALSE(device_api_op_long_running());
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    device_api_op_end(DEVICE_API_OP_RADIO_MOVE); /* a late move end does not free an OTA */
    TEST_ASSERT_TRUE(device_api_op_long_running());
    release_all();
}

TEST_CASE("On a move holding the shared slot: device_api_op_holds says so, and only for the move", "[device_api]")
{
    release_all();
    TEST_ASSERT_FALSE(device_api_op_holds(DEVICE_API_OP_RADIO_MOVE));
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_RADIO_MOVE));
    TEST_ASSERT_TRUE(device_api_op_holds(DEVICE_API_OP_RADIO_MOVE));
    TEST_ASSERT_FALSE(device_api_op_holds(DEVICE_API_OP_OTA));
    release_all();
    TEST_ASSERT_TRUE(device_api_op_try_begin(DEVICE_API_OP_OTA));
    TEST_ASSERT_FALSE(device_api_op_holds(DEVICE_API_OP_RADIO_MOVE));
    release_all();
}
