#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "usb_serial_json.h"

static void defaults(usb_serial_settings_t *s, usb_serial_status_t *st)
{
    usb_serial_settings_default(s);
    memset(st, 0, sizeof(*st));
}

/* ---- device_api_usb_serial_json ---- */

TEST_CASE("USB serial GET, no dongle and no client: dongle and client are null", "[device_api]")
{
    usb_serial_settings_t s;
    usb_serial_status_t st;
    char buf[512];

    defaults(&s, &st);
    TEST_ASSERT_GREATER_THAN(0, device_api_usb_serial_json(buf, sizeof(buf), &s, &st));
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":false,\"port\":6638,\"baud\":115200,\"flow_control\":false,"
                             "\"dongle\":null,\"client\":null,\"bytes_to_dongle\":0,\"bytes_from_dongle\":0,"
                             "\"discarded_from_client\":0,\"overflow_disconnects\":0,\"refused_connections\":0}",
                             buf);
}

TEST_CASE("USB serial GET, an open cp210x dongle and a client: the spec's body", "[device_api]")
{
    usb_serial_settings_t s;
    usb_serial_status_t st;
    char buf[512];

    defaults(&s, &st);
    s.enabled = true;
    st.dongle = (usb_serial_dongle_t) {
        .present = true, .vid = 0x10c4, .pid = 0xea60, .driver = USB_SERIAL_DRV_CP210X, .open = true
    };
    st.client.present = true;
    strcpy(st.client.peer, "192.168.10.23");
    st.client.since_s = 812;
    st.bytes_to_dongle = 12345;
    st.bytes_from_dongle = 67890;
    TEST_ASSERT_GREATER_THAN(0, device_api_usb_serial_json(buf, sizeof(buf), &s, &st));
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":true,\"port\":6638,\"baud\":115200,\"flow_control\":false,"
                             "\"dongle\":{\"vid\":\"10c4\",\"pid\":\"ea60\",\"driver\":\"cp210x\",\"open\":true,"
                             "\"flow_control\":false,\"error\":null},"
                             "\"client\":{\"peer\":\"192.168.10.23\",\"since_s\":812},"
                             "\"bytes_to_dongle\":12345,\"bytes_from_dongle\":67890,\"discarded_from_client\":0,"
                             "\"overflow_disconnects\":0,\"refused_connections\":0}",
                             buf);
}

TEST_CASE("USB serial GET, an unsupported device: driver null, closed", "[device_api]")
{
    usb_serial_settings_t s;
    usb_serial_status_t st;
    char buf[512];

    defaults(&s, &st);
    st.dongle = (usb_serial_dongle_t) {
        .present = true, .vid = 0x0403, .pid = 0x6001, .driver = USB_SERIAL_DRV_NONE
    };
    TEST_ASSERT_GREATER_THAN(0, device_api_usb_serial_json(buf, sizeof(buf), &s, &st));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dongle\":{\"vid\":\"0403\",\"pid\":\"6001\",\"driver\":null,\"open\":false,"
                                "\"flow_control\":false,\"error\":null}"));
}

TEST_CASE("USB serial GET, a dongle error: error is the text", "[device_api]")
{
    usb_serial_settings_t s;
    usb_serial_status_t st;
    char buf[512];

    defaults(&s, &st);
    st.dongle = (usb_serial_dongle_t) {
        .present = true, .vid = 0x1a86, .pid = 0x7523, .driver = USB_SERIAL_DRV_CH34X, .open = true
    };
    strcpy(st.dongle.error, "flow control not supported by ch34x");
    TEST_ASSERT_GREATER_THAN(0, device_api_usb_serial_json(buf, sizeof(buf), &s, &st));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"driver\":\"ch34x\",\"open\":true,\"flow_control\":false,"
                                "\"error\":\"flow control not supported by ch34x\"}"));
}

TEST_CASE("USB serial GET, buffer too small: -1", "[device_api]")
{
    usb_serial_settings_t s;
    usb_serial_status_t st;
    char buf[64];

    defaults(&s, &st);
    TEST_ASSERT_EQUAL(-1, device_api_usb_serial_json(buf, sizeof(buf), &s, &st));
}

/* ---- device_api_usb_serial_parse ---- */

TEST_CASE("USB serial PUT, a subset: changes only that field", "[device_api]")
{
    usb_serial_settings_t s, before;
    bool kick = true;

    usb_serial_settings_default(&s);
    before = s;
    TEST_ASSERT_EQUAL(ESP_OK, device_api_usb_serial_parse("{\"baud\":460800}", &s, &kick));
    TEST_ASSERT_EQUAL_UINT32(460800, s.baud);
    TEST_ASSERT_EQUAL(before.enabled, s.enabled);
    TEST_ASSERT_EQUAL_UINT16(before.port, s.port);
    TEST_ASSERT_EQUAL(before.flow_control, s.flow_control);
    TEST_ASSERT_FALSE(kick);
}

TEST_CASE("USB serial PUT, every setting: all parsed", "[device_api]")
{
    usb_serial_settings_t s;
    bool kick = true;

    usb_serial_settings_default(&s);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_usb_serial_parse(
                          "{\"enabled\":true,\"port\":7000,\"baud\":921600,\"flow_control\":true}", &s, &kick));
    TEST_ASSERT_TRUE(s.enabled);
    TEST_ASSERT_EQUAL_UINT16(7000, s.port);
    TEST_ASSERT_EQUAL_UINT32(921600, s.baud);
    TEST_ASSERT_TRUE(s.flow_control);
    TEST_ASSERT_FALSE(kick);
}

TEST_CASE("USB serial PUT, kick true: sets kick, changes no setting", "[device_api]")
{
    usb_serial_settings_t s, before;
    bool kick = false;

    usb_serial_settings_default(&s);
    before = s;
    TEST_ASSERT_EQUAL(ESP_OK, device_api_usb_serial_parse("{\"kick\":true}", &s, &kick));
    TEST_ASSERT_TRUE(kick);
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

TEST_CASE("USB serial PUT, a port out of range or not whole: 0, refused by the check", "[device_api]")
{
    usb_serial_settings_t s;
    bool kick;

    usb_serial_settings_default(&s);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_usb_serial_parse("{\"port\":70000}", &s, &kick));
    TEST_ASSERT_EQUAL_UINT16(0, s.port);
    TEST_ASSERT_EQUAL(USB_SERIAL_BAD_PORT, usb_serial_settings_check(&s));
    usb_serial_settings_default(&s);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_usb_serial_parse("{\"port\":6638.5}", &s, &kick));
    TEST_ASSERT_EQUAL(USB_SERIAL_BAD_PORT, usb_serial_settings_check(&s));
}

TEST_CASE("USB serial PUT, a wrong type: invalid, settings unchanged", "[device_api]")
{
    usb_serial_settings_t s, before;
    bool kick = false;

    usb_serial_settings_default(&s);
    before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("{\"port\":\"6638\"}", &s, &kick));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("{\"enabled\":1}", &s, &kick));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("{\"kick\":\"yes\"}", &s, &kick));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_api_usb_serial_parse("{\"baud\":460800,\"flow_control\":\"on\"}", &s, &kick));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

TEST_CASE("USB serial PUT, invalid JSON or not an object: invalid", "[device_api]")
{
    usb_serial_settings_t s;
    bool kick;

    usb_serial_settings_default(&s);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("{\"baud\":", &s, &kick));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("[true]", &s, &kick));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_usb_serial_parse("6638", &s, &kick));
}
