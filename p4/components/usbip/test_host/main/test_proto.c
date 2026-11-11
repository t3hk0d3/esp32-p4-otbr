#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "usbip_proto.h"

static usbip_dev_rec_t sample_dev(void)
{
    usbip_dev_rec_t d = {0};

    strcpy(d.path, "/sys/devices/platform/esp32p4-otg/usb1/1-1");
    strcpy(d.busid, "1-1");
    d.busnum = 1;
    d.devnum = 2;
    d.speed = 3;
    d.vid = 0x303a;
    d.pid = 0x4001;
    d.bcd_device = 0x0102;
    d.dev_class = 0xef;
    d.dev_subclass = 2;
    d.dev_proto = 1;
    d.config_value = 1;
    d.num_configs = 1;
    d.num_ifaces = 1;
    d.ifaces[0] = (usbip_iface_rec_t) {
        .cls = 0xff, .subcls = 0x01, .proto = 0x02
    };
    return d;
}

/* ---- usbip_proto_parse_op_header ---- */

TEST_CASE("On version 0x0111 and a known code: returns ESP_OK with the code", "[usbip]")
{
    const uint8_t devlist[8] = {0x01, 0x11, 0x80, 0x05, 0, 0, 0, 0};
    const uint8_t import[8] = {0x01, 0x11, 0x80, 0x03, 0, 0, 0, 0};
    uint16_t code = 0;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_proto_parse_op_header(devlist, &code));
    TEST_ASSERT_EQUAL_HEX16(USBIP_OP_REQ_DEVLIST, code);
    TEST_ASSERT_EQUAL(ESP_OK, usbip_proto_parse_op_header(import, &code));
    TEST_ASSERT_EQUAL_HEX16(USBIP_OP_REQ_IMPORT, code);
}

TEST_CASE("On another version: returns ESP_ERR_INVALID_VERSION", "[usbip]")
{
    const uint8_t hdr[8] = {0x01, 0x10, 0x80, 0x05, 0, 0, 0, 0};
    uint16_t code = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, usbip_proto_parse_op_header(hdr, &code));
}

TEST_CASE("On an unknown code: returns ESP_ERR_NOT_SUPPORTED", "[usbip]")
{
    const uint8_t hdr[8] = {0x01, 0x11, 0x80, 0x06, 0, 0, 0, 0};
    uint16_t code = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, usbip_proto_parse_op_header(hdr, &code));
}

/* ---- usbip_proto_encode_devlist ---- */

TEST_CASE("With a device: writes one 312-byte record and one 4-byte record per interface, big-endian", "[usbip]")
{
    usbip_dev_rec_t d = sample_dev();
    uint8_t buf[512];

    d.num_ifaces = 2;
    d.ifaces[1] = (usbip_iface_rec_t) {
        .cls = 0x03, .subcls = 0x04, .proto = 0x05
    };
    memset(buf, 0xaa, sizeof(buf));
    TEST_ASSERT_EQUAL(12 + 312 + 8, usbip_proto_encode_devlist(&d, buf, sizeof(buf)));
    const uint8_t head[12] = {0x01, 0x11, 0x00, 0x05, 0, 0, 0, 0, 0, 0, 0, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, buf, 12);
    TEST_ASSERT_EQUAL_STRING(d.path, (const char *)buf + 12);
    TEST_ASSERT_EQUAL_STRING("1-1", (const char *)buf + 12 + 256);
    const uint8_t nums[12] = {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nums, buf + 12 + 288, 12);
    const uint8_t ids[6] = {0x30, 0x3a, 0x40, 0x01, 0x01, 0x02};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ids, buf + 12 + 300, 6);
    const uint8_t tail[6] = {0xef, 2, 1, 1, 1, 2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tail, buf + 12 + 306, 6);
    const uint8_t ifs[8] = {0xff, 0x01, 0x02, 0, 0x03, 0x04, 0x05, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ifs, buf + 12 + 312, 8);
}

TEST_CASE("With a 1-interface device: the reply is 12 + 312 + 4 bytes", "[usbip]")
{
    usbip_dev_rec_t d = sample_dev();
    uint8_t buf[512];

    TEST_ASSERT_EQUAL(12 + 312 + 4, usbip_proto_encode_devlist(&d, buf, sizeof(buf)));
}

TEST_CASE("Without a device: writes a header with zero devices", "[usbip]")
{
    uint8_t buf[32];
    const uint8_t want[12] = {0x01, 0x11, 0x00, 0x05, 0, 0, 0, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(12, usbip_proto_encode_devlist(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, 12);
}

TEST_CASE("When cap is too small: returns 0", "[usbip]")
{
    usbip_dev_rec_t d = sample_dev();
    uint8_t buf[512];

    TEST_ASSERT_EQUAL(0, usbip_proto_encode_devlist(&d, buf, 12 + 312 + 3));
    TEST_ASSERT_EQUAL(0, usbip_proto_encode_devlist(NULL, buf, 11));
}

/* ---- usbip_proto_encode_import_reply ---- */

TEST_CASE("On status 0: writes the header and the 312-byte record", "[usbip]")
{
    usbip_dev_rec_t d = sample_dev();
    uint8_t buf[400];
    const uint8_t head[8] = {0x01, 0x11, 0x00, 0x03, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(8 + 312, usbip_proto_encode_import_reply(0, &d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, buf, 8);
    TEST_ASSERT_EQUAL_STRING("1-1", (const char *)buf + 8 + 256);
    TEST_ASSERT_EQUAL_HEX8(1, buf[8 + 311]);
    TEST_ASSERT_EQUAL(0, usbip_proto_encode_import_reply(0, &d, buf, 8 + 311));
}

TEST_CASE("On another status: writes the header only", "[usbip]")
{
    usbip_dev_rec_t d = sample_dev();
    uint8_t buf[400];
    const uint8_t head[8] = {0x01, 0x11, 0x00, 0x03, 0, 0, 0, 0x02};

    TEST_ASSERT_EQUAL(8, usbip_proto_encode_import_reply(2, &d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, buf, 8);
    TEST_ASSERT_EQUAL(8, usbip_proto_encode_import_reply(4, NULL, buf, 8));
}

/* ---- usbip_proto_parse_cmd ---- */

TEST_CASE("On CMD_SUBMIT: fills every field, big-endian, setup bytes as sent", "[usbip]")
{
    const uint8_t hdr[48] = {0, 0, 0, 1,                  /* command */
                             0, 0, 0, 7,                  /* seqnum */
                             0, 1, 0, 2,                  /* devid */
                             0, 0, 0, 1,                  /* direction IN */
                             0, 0, 0, 1,                  /* ep */
                             0, 0, 0, 0x41,               /* flags */
                             0, 0, 0, 64,                 /* length */
                             0xff, 0xff, 0xff, 0xff,      /* start_frame */
                             0xff, 0xff, 0xff, 0xff,      /* num_packets */
                             0, 0, 0, 8,                  /* interval */
                             0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00
                            };
    usbip_cmd_t c;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_proto_parse_cmd(hdr, &c));
    TEST_ASSERT_EQUAL_UINT32(USBIP_CMD_SUBMIT, c.command);
    TEST_ASSERT_EQUAL_UINT32(7, c.seqnum);
    TEST_ASSERT_EQUAL_HEX32(0x00010002, c.devid);
    TEST_ASSERT_EQUAL_UINT32(USBIP_DIR_IN, c.direction);
    TEST_ASSERT_EQUAL_UINT32(1, c.ep);
    TEST_ASSERT_EQUAL_HEX32(USBIP_URB_SHORT_NOT_OK | USBIP_URB_ZERO_PACKET, c.submit.flags);
    TEST_ASSERT_EQUAL_INT32(64, c.submit.length);
    TEST_ASSERT_EQUAL_INT32(-1, c.submit.start_frame);
    TEST_ASSERT_EQUAL_INT32(-1, c.submit.num_packets);
    TEST_ASSERT_EQUAL_INT32(8, c.submit.interval);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hdr + 40, c.submit.setup, 8);
}

TEST_CASE("On CMD_UNLINK: fills the unlink seqnum", "[usbip]")
{
    uint8_t hdr[48] = {0, 0, 0, 2, 0, 0, 0, 9, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
    usbip_cmd_t c;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_proto_parse_cmd(hdr, &c));
    TEST_ASSERT_EQUAL_UINT32(USBIP_CMD_UNLINK, c.command);
    TEST_ASSERT_EQUAL_UINT32(9, c.seqnum);
    TEST_ASSERT_EQUAL_UINT32(7, c.unlink.unlink_seqnum);
}

TEST_CASE("On another command: returns ESP_ERR_INVALID_ARG", "[usbip]")
{
    uint8_t hdr[48] = {0, 0, 0, 3};
    usbip_cmd_t c;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, usbip_proto_parse_cmd(hdr, &c));
    hdr[3] = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, usbip_proto_parse_cmd(hdr, &c));
}

/* ---- usbip_proto_encode_ret_submit / usbip_proto_encode_ret_unlink ---- */

TEST_CASE("RET_SUBMIT: writes a 48-byte header with the command, seqnum and status", "[usbip]")
{
    const usbip_ret_submit_t r = {.seqnum = 7, .status = -32, .actual_length = 18, .start_frame = 0,
                                  .num_packets = -1, .error_count = 0
                                 };
    uint8_t hdr[48];
    const uint8_t want_status[4] = {0xff, 0xff, 0xff, 0xe0};

    memset(hdr, 0xaa, sizeof(hdr));
    usbip_proto_encode_ret_submit(&r, hdr);
    TEST_ASSERT_EQUAL_HEX8(USBIP_RET_SUBMIT, hdr[3]);
    TEST_ASSERT_EQUAL_HEX8(7, hdr[7]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_status, hdr + 20, 4);
    TEST_ASSERT_EQUAL_HEX8(18, hdr[27]);
    TEST_ASSERT_EQUAL_HEX8(0xff, hdr[35]);
    for (int i = 40; i < 48; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, hdr[i]);
    }
}

TEST_CASE("RET_UNLINK: puts the status at offset 20", "[usbip]")
{
    uint8_t hdr[48];
    const uint8_t want_status[4] = {0xff, 0xff, 0xff, 0x98};

    memset(hdr, 0xaa, sizeof(hdr));
    usbip_proto_encode_ret_unlink(9, -104, hdr);
    TEST_ASSERT_EQUAL_HEX8(USBIP_RET_UNLINK, hdr[3]);
    TEST_ASSERT_EQUAL_HEX8(9, hdr[7]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_status, hdr + 20, 4);
    TEST_ASSERT_EQUAL_HEX8(0, hdr[24]);
    TEST_ASSERT_EQUAL_HEX8(0, hdr[47]);
}

/* ---- usbip_proto_first_byte_ok ---- */

TEST_CASE("On 0x01: returns true", "[usbip]")
{
    TEST_ASSERT_TRUE(usbip_proto_first_byte_ok(0x01));
}

TEST_CASE("On any letter or 0x00: returns false", "[usbip]")
{
    TEST_ASSERT_FALSE(usbip_proto_first_byte_ok(0x00));
    TEST_ASSERT_FALSE(usbip_proto_first_byte_ok('G'));
    TEST_ASSERT_FALSE(usbip_proto_first_byte_ok('a'));
    TEST_ASSERT_FALSE(usbip_proto_first_byte_ok(0x16));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
