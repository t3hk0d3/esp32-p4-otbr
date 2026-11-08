#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "cJSON.h"
#include "zigbee_json.h"

static zigbee_status_t running(void)
{
    zigbee_status_t st = {0};

    strcpy(st.channel, "usb-303a:1001");
    strcpy(st.firmware, "spinel 1.2 efr32");
    st.state = ZIGBEE_STATE_RUNNING;
    st.has_client = true;
    strcpy(st.client_address, "192.168.1.20:51234");
    st.client_since = 1790000000;
    st.has_radio_config = true;
    st.radio = (zigbee_radio_config_t) {
        .channel = 15, .tx_power_dbm = 8, .pan_id = 0x1a62, .short_addr = 0x0000,
        .ext_addr = {0x00, 0x12, 0x4b, 0x00, 0xab, 0xcd, 0xef, 0x01},
    };
    st.counters.rx = 10;
    st.counters.rx_dropped = 1;
    st.counters.tx = 9;
    st.counters.tx_no_ack = 2;
    st.counters.tx_cca = 3;
    st.counters.tx_failed = 4;
    st.counters.energy_scans = 5;
    st.counters.rcp_resets = 6;
    st.counters.frames_in = 7;
    st.counters.frames_out = 8;
    st.heap.current = 1000;
    st.heap.peak = 2000;
    st.stack_started = true;
    st.started = 1789999000;
    return st;
}

static char body[1536];
static size_t body_len;
static cJSON *json;

static void render(const zigbee_status_t *st)
{
    cJSON_Delete(json);
    TEST_ASSERT_EQUAL_INT(ESP_OK, zigbee_json_status(st, body, sizeof(body), &body_len));
    TEST_ASSERT_EQUAL_UINT(strlen(body), body_len);
    json = cJSON_Parse(body);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, body);
}

static const cJSON *field(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    TEST_ASSERT_NOT_NULL_MESSAGE(item, key);
    return item;
}

TEST_CASE("zigbee_json_status running: channel and firmware are strings", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    TEST_ASSERT_EQUAL_STRING("usb-303a:1001", field(json, "channel")->valuestring);
    TEST_ASSERT_EQUAL_STRING("spinel 1.2 efr32", field(json, "firmware")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", field(json, "reason")->valuestring);
}

TEST_CASE("zigbee_json_status state: each state has its name", "[device_api]")
{
    const char *names[] = {"idle", "starting", "running", "no_radio", "incompatible"};
    const zigbee_state_t states[] = {ZIGBEE_STATE_IDLE, ZIGBEE_STATE_STARTING, ZIGBEE_STATE_RUNNING,
                                     ZIGBEE_STATE_NO_RADIO, ZIGBEE_STATE_INCOMPATIBLE
                                    };

    for (int i = 0; i < 5; i++) {
        zigbee_status_t st = running();

        st.state = states[i];
        render(&st);
        TEST_ASSERT_EQUAL_STRING(names[i], field(json, "state")->valuestring);
    }
}

TEST_CASE("zigbee_json_status with a reason: the reason is written", "[device_api]")
{
    zigbee_status_t st = running();

    st.state = ZIGBEE_STATE_INCOMPATIBLE;
    strcpy(st.reason, "spinel protocol 4.2 is not supported");
    render(&st);
    TEST_ASSERT_EQUAL_STRING("spinel protocol 4.2 is not supported", field(json, "reason")->valuestring);
}

TEST_CASE("zigbee_json_status with a client: client has address and since", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    const cJSON *c = field(json, "client");
    TEST_ASSERT_EQUAL_STRING("192.168.1.20:51234", field(c, "address")->valuestring);
    TEST_ASSERT_EQUAL_INT64(1790000000, (int64_t)field(c, "since")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsNull(field(json, "last_client")));
}

TEST_CASE("zigbee_json_status without a client: client is null", "[device_api]")
{
    zigbee_status_t st = running();

    st.has_client = false;
    render(&st);
    TEST_ASSERT_TRUE(cJSON_IsNull(field(json, "client")));
}

TEST_CASE("zigbee_json_status with a last client: last_client has address and left", "[device_api]")
{
    zigbee_status_t st = running();

    st.has_client = false;
    st.has_last_client = true;
    strcpy(st.last_client_address, "10.0.0.7:4000");
    st.last_client_left = 1790000100;
    render(&st);
    const cJSON *c = field(json, "last_client");
    TEST_ASSERT_EQUAL_STRING("10.0.0.7:4000", field(c, "address")->valuestring);
    TEST_ASSERT_EQUAL_INT64(1790000100, (int64_t)field(c, "left")->valuedouble);
}

TEST_CASE("zigbee_json_status with the clock not set: since is 0", "[device_api]")
{
    zigbee_status_t st = running();

    st.client_since = 0;
    render(&st);
    TEST_ASSERT_EQUAL_INT(0, field(field(json, "client"), "since")->valueint);
}

TEST_CASE("zigbee_json_status with a radio config: radio has its fields, the address as hex", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    const cJSON *r = field(json, "radio");
    TEST_ASSERT_EQUAL_INT(15, field(r, "channel")->valueint);
    TEST_ASSERT_EQUAL_INT(0x1a62, field(r, "pan_id")->valueint);
    TEST_ASSERT_EQUAL_INT(0, field(r, "short_address")->valueint);
    TEST_ASSERT_EQUAL_STRING("00124b00abcdef01", field(r, "extended_address")->valuestring);
    TEST_ASSERT_EQUAL_INT(8, field(r, "tx_power_dbm")->valueint);
}

TEST_CASE("zigbee_json_status with a negative TX power: tx_power_dbm is negative", "[device_api]")
{
    zigbee_status_t st = running();

    st.radio.tx_power_dbm = -5;
    render(&st);
    TEST_ASSERT_EQUAL_INT(-5, field(field(json, "radio"), "tx_power_dbm")->valueint);
}

TEST_CASE("zigbee_json_status without a radio config: radio is null", "[device_api]")
{
    zigbee_status_t st = running();

    st.has_radio_config = false;
    render(&st);
    TEST_ASSERT_TRUE(cJSON_IsNull(field(json, "radio")));
}

TEST_CASE("zigbee_json_status: counters by name", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    const cJSON *c = field(json, "counters");
    TEST_ASSERT_EQUAL_INT(10, field(c, "rx")->valueint);
    TEST_ASSERT_EQUAL_INT(1, field(c, "rx_dropped")->valueint);
    TEST_ASSERT_EQUAL_INT(9, field(c, "tx")->valueint);
    TEST_ASSERT_EQUAL_INT(2, field(c, "tx_no_ack")->valueint);
    TEST_ASSERT_EQUAL_INT(3, field(c, "tx_cca")->valueint);
    TEST_ASSERT_EQUAL_INT(4, field(c, "tx_failed")->valueint);
    TEST_ASSERT_EQUAL_INT(5, field(c, "energy_scans")->valueint);
    TEST_ASSERT_EQUAL_INT(6, field(c, "rcp_resets")->valueint);
    TEST_ASSERT_EQUAL_INT(7, field(c, "frames_in")->valueint);
    TEST_ASSERT_EQUAL_INT(8, field(c, "frames_out")->valueint);
}

TEST_CASE("zigbee_json_status with a counter above 2^31: it is written unsigned", "[device_api]")
{
    zigbee_status_t st = running();

    st.counters.rx = 4000000000u;
    render(&st);
    TEST_ASSERT_EQUAL_INT64(4000000000, (int64_t)field(field(json, "counters"), "rx")->valuedouble);
}

TEST_CASE("zigbee_json_status: heap has current and peak", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    TEST_ASSERT_EQUAL_INT(1000, field(field(json, "heap"), "current")->valueint);
    TEST_ASSERT_EQUAL_INT(2000, field(field(json, "heap"), "peak")->valueint);
}

TEST_CASE("zigbee_json_status with the stack started: started is the epoch seconds", "[device_api]")
{
    zigbee_status_t st = running();

    render(&st);
    TEST_ASSERT_EQUAL_INT64(1789999000, (int64_t)field(json, "started")->valuedouble);
}

TEST_CASE("zigbee_json_status started with the clock not set: started is 0, not null", "[device_api]")
{
    zigbee_status_t st = running();

    st.started = 0;
    render(&st);
    TEST_ASSERT_TRUE(cJSON_IsNumber(field(json, "started")));
    TEST_ASSERT_EQUAL_INT(0, field(json, "started")->valueint);
}

TEST_CASE("zigbee_json_status with the stack not started: started is null", "[device_api]")
{
    zigbee_status_t st = running();

    st.stack_started = false;
    st.started = 0;
    render(&st);
    TEST_ASSERT_TRUE(cJSON_IsNull(field(json, "started")));
}

TEST_CASE("zigbee_json_status with a quote in the reason: it is escaped", "[device_api]")
{
    zigbee_status_t st = running();

    strcpy(st.reason, "bad \"thing\"");
    render(&st);
    TEST_ASSERT_EQUAL_STRING("bad \"thing\"", field(json, "reason")->valuestring);
}

TEST_CASE("zigbee_json_status into a buffer that is too small: ESP_ERR_INVALID_SIZE and a terminated prefix", "[device_api]")
{
    zigbee_status_t st = running();
    char small[40];
    size_t len = 99;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, zigbee_json_status(&st, small, sizeof(small), &len));
    TEST_ASSERT_EQUAL_UINT(strlen(small), len);
}

TEST_CASE("zigbee_json_status with the largest strings: fits the documented buffer", "[device_api]")
{
    zigbee_status_t st = running();
    char big[ZIGBEE_JSON_MAX];

    memset(st.channel, 'c', sizeof(st.channel) - 1);
    memset(st.firmware, 'f', sizeof(st.firmware) - 1);
    memset(st.reason, '"', sizeof(st.reason) - 1); // every byte escapes to two
    memset(st.client_address, 'a', sizeof(st.client_address) - 1);
    st.has_last_client = true;
    memset(st.last_client_address, 'l', sizeof(st.last_client_address) - 1);
    st.counters.rx = st.counters.tx = UINT32_MAX;
    st.heap.current = st.heap.peak = SIZE_MAX;
    st.client_since = st.last_client_left = st.started = INT64_MAX;
    st.radio.tx_power_dbm = -128;
    size_t len;

    TEST_ASSERT_EQUAL_INT(ESP_OK, zigbee_json_status(&st, big, sizeof(big), &len));
}
