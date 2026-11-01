#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"
#include "rcp_sync.h"
#include "br_core_mode.h"
#include "ota_bundle.h"

#define OTA_VALIDATE_FAILED 0x1503
#define IMAGE_INVALID 0x2002

TEST_CASE("On an error: the body is JSON with error and detail", "[device_api]")
{
    char buf[128];

    TEST_ASSERT_EQUAL(strlen("{\"error\":\"busy\",\"detail\":\"an update runs\"}"), device_api_error_json(buf, sizeof(buf), "busy", "an update runs"));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"busy\",\"detail\":\"an update runs\"}", buf);
}

TEST_CASE("On a detail with quotes: it is escaped", "[device_api]")
{
    char buf[128];

    device_api_error_json(buf, sizeof(buf), "x", "say \"hi\"");
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"x\",\"detail\":\"say \\\"hi\\\"\"}", buf);
}

TEST_CASE("On a NULL detail: detail is an empty string", "[device_api]")
{
    char buf[64];

    device_api_error_json(buf, sizeof(buf), "x", NULL);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"x\",\"detail\":\"\"}", buf);
}

TEST_CASE("On a buffer too small for the body: returns -1", "[device_api]")
{
    char buf[10];

    TEST_ASSERT_EQUAL(-1, device_api_error_json(buf, sizeof(buf), "busy", "an update runs"));
}

TEST_CASE("On the status codes device_api sends: the status line has the code and a reason", "[device_api]")
{
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", device_api_status_line(400));
    TEST_ASSERT_EQUAL_STRING("404 Not Found", device_api_status_line(404));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", device_api_status_line(409));
    TEST_ASSERT_EQUAL_STRING("413 Payload Too Large", device_api_status_line(413));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", device_api_status_line(500));
    TEST_ASSERT_EQUAL_STRING("502 Bad Gateway", device_api_status_line(502));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", device_api_status_line(503));
}

TEST_CASE("On an OTA that is refused as running or committed: 409", "[device_api]")
{
    TEST_ASSERT_EQUAL(409, device_api_map_ota_error(ESP_ERR_INVALID_STATE).http_status);
    TEST_ASSERT_EQUAL_STRING("busy", device_api_map_ota_error(ESP_ERR_INVALID_STATE).code);
}

TEST_CASE("On an OTA refused because the running image is not confirmed yet: 409 busy", "[device_api]")
{
    TEST_ASSERT_EQUAL(409, device_api_map_ota_error(ESP_BR_OTA_ERR_NOT_CONFIRMED).http_status);
    TEST_ASSERT_EQUAL_STRING("busy", device_api_map_ota_error(ESP_BR_OTA_ERR_NOT_CONFIRMED).code);
}

TEST_CASE("On an OTA refused because the running image is not confirmed yet: the detail tells to retry in up to 60 s", "[device_api]")
{
    TEST_ASSERT_EQUAL_STRING("the running update is not confirmed yet; retry in up to 60 s",
                             device_api_map_ota_error(ESP_BR_OTA_ERR_NOT_CONFIRMED).detail);
}

TEST_CASE("On an OTA with an invalid bundle: 400 invalid_bundle", "[device_api]")
{
    const esp_err_t errs[] = {ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_VERSION, ESP_ERR_INVALID_SIZE, OTA_VALIDATE_FAILED, IMAGE_INVALID};

    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        TEST_ASSERT_EQUAL(400, device_api_map_ota_error(errs[i]).http_status);
        TEST_ASSERT_EQUAL_STRING("invalid_bundle", device_api_map_ota_error(errs[i]).code);
    }
}

TEST_CASE("On an OTA that failed in transfer or memory: 502 transfer_failed and 500 no_memory", "[device_api]")
{
    TEST_ASSERT_EQUAL(502, device_api_map_ota_error(ESP_FAIL).http_status);
    TEST_ASSERT_EQUAL_STRING("transfer_failed", device_api_map_ota_error(ESP_FAIL).code);
    TEST_ASSERT_EQUAL(500, device_api_map_ota_error(ESP_ERR_NO_MEM).http_status);
    TEST_ASSERT_EQUAL_STRING("no_memory", device_api_map_ota_error(ESP_ERR_NO_MEM).code);
}

TEST_CASE("On a push refused in SNIFFER or during another operation: 409", "[device_api]")
{
    TEST_ASSERT_EQUAL(409, device_api_map_push_error(ESP_ERR_INVALID_STATE).http_status);
    TEST_ASSERT_EQUAL_STRING("busy", device_api_map_push_error(ESP_ERR_INVALID_STATE).code);
}

TEST_CASE("On a push with an image that does not fit the C6 slot: 400 image_too_large", "[device_api]")
{
    TEST_ASSERT_EQUAL(400, device_api_map_push_error(ESP_ERR_INVALID_SIZE).http_status);
    TEST_ASSERT_EQUAL_STRING("image_too_large", device_api_map_push_error(ESP_ERR_INVALID_SIZE).code);
}

TEST_CASE("On a push whose image the C6 rejected at END: 400 invalid_image", "[device_api]")
{
    device_api_error_t e = device_api_map_push_error(RCP_SYNC_ERR_IMAGE_REJECTED);

    TEST_ASSERT_EQUAL(400, e.http_status);
    TEST_ASSERT_EQUAL_STRING("invalid_image", e.code);
}

TEST_CASE("On a push that failed in the transfer: 502 push_failed", "[device_api]")
{
    TEST_ASSERT_EQUAL(502, device_api_map_push_error(ESP_ERR_TIMEOUT).http_status);
    TEST_ASSERT_EQUAL(502, device_api_map_push_error(ESP_FAIL).http_status);
    TEST_ASSERT_EQUAL_STRING("push_failed", device_api_map_push_error(ESP_FAIL).code);
}

TEST_CASE("On a recovery without the jumper: 412 no_jumper", "[device_api]")
{
    device_api_error_t e = device_api_map_recover_error(ESP_ERR_NOT_SUPPORTED);

    TEST_ASSERT_EQUAL(412, e.http_status);
    TEST_ASSERT_EQUAL_STRING("no_jumper", e.code);
    TEST_ASSERT_EQUAL_STRING("412 Precondition Failed", device_api_status_line(412));
}

TEST_CASE("On a recovery refused in SNIFFER or during another operation: 409 busy", "[device_api]")
{
    TEST_ASSERT_EQUAL(409, device_api_map_recover_error(ESP_ERR_INVALID_STATE).http_status);
}

TEST_CASE("On a recovery that failed: 502 recovery_failed", "[device_api]")
{
    device_api_error_t e = device_api_map_recover_error(ESP_FAIL);

    TEST_ASSERT_EQUAL(502, e.http_status);
    TEST_ASSERT_EQUAL_STRING("recovery_failed", e.code);
    TEST_ASSERT_EQUAL(500, device_api_map_recover_error(ESP_ERR_NO_MEM).http_status);
}

TEST_CASE("On an invalid network config: 400 invalid_config with the field as detail", "[device_api]")
{
    char buf[128];
    const device_api_error_t e = device_api_map_network_set_error(ESP_ERR_INVALID_ARG);

    TEST_ASSERT_EQUAL(400, e.http_status);
    TEST_ASSERT_EQUAL_STRING("invalid_config", e.code);
    TEST_ASSERT_GREATER_THAN(0, device_api_error_json(buf, sizeof(buf), e.code, "vlan"));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"invalid_config\",\"detail\":\"vlan\"}", buf);
}

TEST_CASE("On a network config that cannot be stored or applied: 500 store_failed, 503 before netcfg runs", "[device_api]")
{
    TEST_ASSERT_EQUAL(500, device_api_map_network_set_error(ESP_FAIL).http_status);
    TEST_ASSERT_EQUAL_STRING("store_failed", device_api_map_network_set_error(ESP_FAIL).code);
    TEST_ASSERT_EQUAL(503, device_api_map_network_set_error(ESP_ERR_INVALID_STATE).http_status);
    TEST_ASSERT_EQUAL(500, device_api_map_network_set_error(ESP_ERR_NO_MEM).http_status);
}

TEST_CASE("On a confirm without a trial: 409 no_trial", "[device_api]")
{
    const device_api_error_t e = device_api_map_network_confirm_error(ESP_ERR_INVALID_STATE);

    TEST_ASSERT_EQUAL(409, e.http_status);
    TEST_ASSERT_EQUAL_STRING("no_trial", e.code);
}

TEST_CASE("On a confirm on the wrong interface: 403 wrong_interface", "[device_api]")
{
    const device_api_error_t e = device_api_map_network_confirm_error(ESP_ERR_NOT_ALLOWED);

    TEST_ASSERT_EQUAL(403, e.http_status);
    TEST_ASSERT_EQUAL_STRING("wrong_interface", e.code);
    TEST_ASSERT_EQUAL_STRING("403 Forbidden", device_api_status_line(403));
}

TEST_CASE("On a confirm that cannot be stored: 500 store_failed", "[device_api]")
{
    TEST_ASSERT_EQUAL(500, device_api_map_network_confirm_error(ESP_FAIL).http_status);
    TEST_ASSERT_EQUAL(500, device_api_map_network_confirm_error(ESP_ERR_NO_MEM).http_status);
}

/* ---- POST /device/radio/recover reply ---- */

TEST_CASE("On a recovery that worked and the restart found no RCP firmware: 200 recovered, rcp \"no rcp firmware\"", "[device_api]")
{
    char buf[384];
    int status = 0;

    TEST_ASSERT_GREATER_THAN(0, device_api_recover_reply(ESP_OK, "", BR_MODE_RADIO_FAULT, "no rcp firmware", true, "", buf, sizeof(buf), &status));
    TEST_ASSERT_EQUAL(200, status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"recovered\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rcp\":\"no rcp firmware\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"RADIO_FAULT\""));
}

TEST_CASE("On a recovery that worked and the RCP in its slot runs: 200 recovered with the installed version", "[device_api]")
{
    char buf[384];
    int status = 0;

    TEST_ASSERT_GREATER_THAN(0, device_api_recover_reply(ESP_OK, "", BR_MODE_BR_RUNNING, "", true, "20261101T1-abc1234", buf, sizeof(buf), &status));
    TEST_ASSERT_EQUAL(200, status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rcp\":\"20261101T1-abc1234\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"BR_RUNNING\""));
}

TEST_CASE("On a recovery that worked and the RCP version is unreadable: 200 recovered, rcp \"unknown\"", "[device_api]")
{
    char buf[384];
    int status = 0;

    TEST_ASSERT_GREATER_THAN(0, device_api_recover_reply(ESP_OK, "", BR_MODE_RADIO_FAULT, "", false, "", buf, sizeof(buf), &status));
    TEST_ASSERT_EQUAL(200, status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rcp\":\"unknown\""));
}

TEST_CASE("On a recovery that failed: 502 with the recovery_failed body and the reason", "[device_api]")
{
    char buf[384];
    int status = 0;

    device_api_recover_reply(ESP_FAIL, "ROM recovery of the C6 base image failed", BR_MODE_RADIO_FAULT, "", true, "", buf, sizeof(buf), &status);
    TEST_ASSERT_EQUAL(502, status);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"recovery_failed\",\"detail\":\"ROM recovery of the C6 base image failed\"}", buf);
}

TEST_CASE("On a recovery refused (no jumper, busy): the mapped status and code", "[device_api]")
{
    char buf[384];
    int status = 0;

    device_api_recover_reply(ESP_ERR_NOT_SUPPORTED, "", BR_MODE_BR_RUNNING, "", true, "", buf, sizeof(buf), &status);
    TEST_ASSERT_EQUAL(412, status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"error\":\"no_jumper\""));
    device_api_recover_reply(ESP_ERR_INVALID_STATE, "", BR_MODE_BR_RUNNING, "", true, "", buf, sizeof(buf), &status);
    TEST_ASSERT_EQUAL(409, status);
}

TEST_CASE("On a reply buffer too small: returns -1", "[device_api]")
{
    char buf[20];
    int status = 0;

    TEST_ASSERT_EQUAL(-1, device_api_recover_reply(ESP_OK, "", BR_MODE_RADIO_FAULT, "", true, "", buf, sizeof(buf), &status));
}

TEST_CASE("On a recovery that worked with an RCP version known but RADIO_FAULT: detail names the fault, not a running RCP", "[device_api]")
{
    char buf[384];
    int status = 0;

    device_api_recover_reply(ESP_OK, "", BR_MODE_RADIO_FAULT, "rcp incompatible", true, "v1", buf, sizeof(buf), &status);
    TEST_ASSERT_EQUAL(200, status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rcp\":\"v1\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "rcp incompatible"));
    TEST_ASSERT_NULL(strstr(buf, "is running"));
}

TEST_CASE("On a recovery that worked without RCP firmware: detail tells to upload it", "[device_api]")
{
    char buf[384];
    int status = 0;

    device_api_recover_reply(ESP_OK, "", BR_MODE_RADIO_FAULT, "no rcp firmware", true, "", buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL(strstr(buf, "upload c6_rcp.bin"));
}

TEST_CASE("On a capture without channel and an unreadable OpenThread state: 503 busy (retry)", "[device_api]")
{
    device_api_error_t e = device_api_pcap_default_channel_error(false, false);

    TEST_ASSERT_EQUAL(503, e.http_status);
    TEST_ASSERT_EQUAL_STRING("busy", e.code);
}

TEST_CASE("On a capture without channel and an unreadable OpenThread state: a stored dataset does not matter", "[device_api]")
{
    TEST_ASSERT_EQUAL(503, device_api_pcap_default_channel_error(false, true).http_status);
}

TEST_CASE("On a capture without channel and no active dataset: 400 no_channel", "[device_api]")
{
    device_api_error_t e = device_api_pcap_default_channel_error(true, false);

    TEST_ASSERT_EQUAL(400, e.http_status);
    TEST_ASSERT_EQUAL_STRING("no_channel", e.code);
}

TEST_CASE("On a capture without channel and an active dataset: no error", "[device_api]")
{
    TEST_ASSERT_EQUAL(0, device_api_pcap_default_channel_error(true, true).http_status);
}
