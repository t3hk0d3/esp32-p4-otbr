#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "br_core_mode.h"
#include "device_api_priv.h"

// Reply of POST /device/topology for `start_err` in BR_RUNNING with a free long-op slot.
static device_api_topology_reply_t reply_for(esp_err_t start_err)
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(start_err, BR_MODE_BR_RUNNING, false, "", false, "", &r);
    return r;
}

TEST_CASE("On ESP_OK: 202 without an error", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_OK);

    TEST_ASSERT_EQUAL_INT(202, r.status);
    TEST_ASSERT_NULL(r.error);
}

TEST_CASE("On ESP_ERR_INVALID_STATE with BR_RUNNING (a scan runs): 202 without an error", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_ERR_INVALID_STATE);

    TEST_ASSERT_EQUAL_INT(202, r.status);
    TEST_ASSERT_NULL(r.error);
}

TEST_CASE("On the mode STARTING: 409 busy, detail mode STARTING", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_STARTING, false, "", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mode STARTING", r.detail);
}

TEST_CASE("On the mode SNIFFER: 409 busy, detail mode SNIFFER", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_SNIFFER, false, "", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mode SNIFFER", r.detail);
}

TEST_CASE("On the mode RADIO_FAULT: 409 busy, detail mode RADIO_FAULT", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_RADIO_FAULT, false, "", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mode RADIO_FAULT", r.detail);
}

TEST_CASE("On a mode other than BR_RUNNING with the slot held: the mode wins", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_SNIFFER, true, "an update is running", true, "a capture", &r);
    TEST_ASSERT_EQUAL_STRING("mode SNIFFER", r.detail);
}

TEST_CASE("On the long-op slot held: 409 busy with the slot's detail", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_BR_RUNNING, true, "an update is running", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("an update is running", r.detail);
}

TEST_CASE("On the pcap slot held: 409 busy with the pcap slot's detail", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    device_api_topology_start_reply(ESP_OK, BR_MODE_BR_RUNNING, false, "", true,
                                    "a capture stream is already running", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("a capture stream is already running", r.detail);
}

TEST_CASE("On ESP_ERR_NOT_ALLOWED (the mode left BR_RUNNING before the start): 409 busy, detail mode <MODE> of the "
          "mode passed", "[device_api][topology]")
{
    device_api_topology_reply_t r;

    // The handler re-reads the mode after the refusal and passes it.
    device_api_topology_start_reply(ESP_ERR_NOT_ALLOWED, BR_MODE_SNIFFER, false, "", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mode SNIFFER", r.detail);

    // Even when the mode passed reads BR_RUNNING again by then.
    device_api_topology_start_reply(ESP_ERR_NOT_ALLOWED, BR_MODE_BR_RUNNING, false, "", false, "", &r);
    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mode BR_RUNNING", r.detail);
}

TEST_CASE("On ESP_ERR_NOT_FINISHED: 409 busy, detail mesh diagnostics are in use", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_ERR_NOT_FINISHED);

    TEST_ASSERT_EQUAL_INT(409, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("mesh diagnostics are in use", r.detail);
}

TEST_CASE("On ESP_ERR_NO_MEM: 500 no_memory", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_ERR_NO_MEM);

    TEST_ASSERT_EQUAL_INT(500, r.status);
    TEST_ASSERT_EQUAL_STRING("no_memory", r.error);
}

TEST_CASE("On ESP_ERR_TIMEOUT: 503 busy, detail OpenThread is busy, retry", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_ERR_TIMEOUT);

    TEST_ASSERT_EQUAL_INT(503, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("OpenThread is busy, retry", r.detail);
}

TEST_CASE("On ESP_ERR_NOT_SUPPORTED (mesh_scan never initialised): 503 busy, detail topology scan unavailable",
          "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_ERR_NOT_SUPPORTED);

    TEST_ASSERT_EQUAL_INT(503, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("topology scan unavailable", r.detail);
}

TEST_CASE("On ESP_FAIL: 503 busy, detail OpenThread is busy, retry", "[device_api][topology]")
{
    const device_api_topology_reply_t r = reply_for(ESP_FAIL);

    TEST_ASSERT_EQUAL_INT(503, r.status);
    TEST_ASSERT_EQUAL_STRING("busy", r.error);
    TEST_ASSERT_EQUAL_STRING("OpenThread is busy, retry", r.detail);
}

TEST_CASE("On the status 202: the status line is 202 Accepted", "[device_api][topology]")
{
    TEST_ASSERT_EQUAL_STRING("202 Accepted", device_api_status_line(202));
}
