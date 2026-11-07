// Host tests of the C6 transport's pure parts: the SPI header (OpenThread's spi_frame.hpp layout), the check of one
// transaction, its length, the C6's availability from rcp_sync's probe and the job refusal rule.
#include <string.h>
#include "radio_mux_core.h"
#include "unity.h"

#define FRAME_MAX 2048

/* ---- radio_mux_spi_hdr_encode ---- */

TEST_CASE("On a header without the reset bit: writes flag 0x02, accept and data length little-endian", "[radio_mux]")
{
    const radio_mux_spi_hdr_t h = {.reset = false, .accept_len = 0x1234, .data_len = 0x0567};
    const uint8_t expected[RADIO_MUX_SPI_HDR_LEN] = {0x02, 0x34, 0x12, 0x67, 0x05};
    uint8_t out[RADIO_MUX_SPI_HDR_LEN];

    memset(out, 0xEE, sizeof(out));
    radio_mux_spi_hdr_encode(&h, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
}

TEST_CASE("On a header with the reset bit: writes flag 0x82", "[radio_mux]")
{
    const radio_mux_spi_hdr_t h = {.reset = true, .accept_len = 2048, .data_len = 0};
    const uint8_t expected[RADIO_MUX_SPI_HDR_LEN] = {0x82, 0x00, 0x08, 0x00, 0x00};
    uint8_t out[RADIO_MUX_SPI_HDR_LEN];

    radio_mux_spi_hdr_encode(&h, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
}

/* ---- radio_mux_spi_hdr_decode ---- */

TEST_CASE("On a valid header: returns true with the reset bit, accept and data length", "[radio_mux]")
{
    const uint8_t in[RADIO_MUX_SPI_HDR_LEN] = {0x82, 0x34, 0x12, 0x67, 0x05};
    radio_mux_spi_hdr_t h = {0};

    TEST_ASSERT_TRUE(radio_mux_spi_hdr_decode(in, &h));
    TEST_ASSERT_TRUE(h.reset);
    TEST_ASSERT_EQUAL_UINT16(0x1234, h.accept_len);
    TEST_ASSERT_EQUAL_UINT16(0x0567, h.data_len);
}

TEST_CASE("On a valid header without the reset bit and other flag bits set: returns true, reset false", "[radio_mux]")
{
    const uint8_t in[RADIO_MUX_SPI_HDR_LEN] = {0x7E, 0x00, 0x00, 0x10, 0x00};
    radio_mux_spi_hdr_t h = {.reset = true};

    TEST_ASSERT_TRUE(radio_mux_spi_hdr_decode(in, &h));
    TEST_ASSERT_FALSE(h.reset);
    TEST_ASSERT_EQUAL_UINT16(16, h.data_len);
}

TEST_CASE("On a corrupt pattern (0xFF, 0x00, 0x01, 0x83): returns false and leaves the output", "[radio_mux]")
{
    const uint8_t flags[] = {0xFF, 0x00, 0x01, 0x83};

    for (size_t i = 0; i < sizeof(flags); i++) {
        const uint8_t in[RADIO_MUX_SPI_HDR_LEN] = {flags[i], 0x10, 0x00, 0x10, 0x00};
        radio_mux_spi_hdr_t h = {.reset = true, .accept_len = 7, .data_len = 9};

        TEST_ASSERT_FALSE(radio_mux_spi_hdr_decode(in, &h));
        TEST_ASSERT_TRUE(h.reset);
        TEST_ASSERT_EQUAL_UINT16(7, h.accept_len);
        TEST_ASSERT_EQUAL_UINT16(9, h.data_len);
    }
}

TEST_CASE("On encode then decode: returns the same header", "[radio_mux]")
{
    const radio_mux_spi_hdr_t h = {.reset = true, .accept_len = 32, .data_len = 1500};
    uint8_t buf[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_hdr_t back = {0};

    radio_mux_spi_hdr_encode(&h, buf);
    TEST_ASSERT_TRUE(radio_mux_spi_hdr_decode(buf, &back));
    TEST_ASSERT_EQUAL(h.reset, back.reset);
    TEST_ASSERT_EQUAL_UINT16(h.accept_len, back.accept_len);
    TEST_ASSERT_EQUAL_UINT16(h.data_len, back.data_len);
}

/* ---- radio_mux_spi_check ---- */

static void rcp_hdr(bool reset, uint16_t accept, uint16_t data, uint8_t out[RADIO_MUX_SPI_HDR_LEN])
{
    const radio_mux_spi_hdr_t h = {.reset = reset, .accept_len = accept, .data_len = data};

    radio_mux_spi_hdr_encode(&h, out);
}

TEST_CASE("On a frame from the RCP within our accept: valid, rx_len its length", "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_result_t r;

    rcp_hdr(false, 2048, 120, in);
    radio_mux_spi_check(in, 0, FRAME_MAX, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.rcp_reset);
    TEST_ASSERT_EQUAL_UINT16(120, r.rx_len);
    TEST_ASSERT_TRUE(r.tx_taken);
}

TEST_CASE("On a frame exactly as long as our accept: rx_len its length", "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_result_t r;

    rcp_hdr(false, 2048, 32, in);
    radio_mux_spi_check(in, 10, 32, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT16(32, r.rx_len);
}

TEST_CASE("On a frame longer than our accept: valid, rx_len 0 (the RCP keeps it for the next transaction)",
          "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_result_t r;

    rcp_hdr(false, 2048, 200, in);
    radio_mux_spi_check(in, 10, 32, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT16(0, r.rx_len);
    TEST_ASSERT_TRUE(r.tx_taken);
}

TEST_CASE("On our frame within the RCP's accept: tx_taken true; above it (accept 0, busy): tx_taken false",
          "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_result_t r;

    rcp_hdr(false, 100, 0, in);
    radio_mux_spi_check(in, 100, 100, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.tx_taken);
    TEST_ASSERT_EQUAL_UINT16(0, r.rx_len);
    rcp_hdr(false, 0, 0, in);
    radio_mux_spi_check(in, 1, 32, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.tx_taken);
}

TEST_CASE("On the RCP's reset bit: rcp_reset true", "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN];
    radio_mux_spi_result_t r;

    rcp_hdr(true, 0, 0, in);
    radio_mux_spi_check(in, 0, FRAME_MAX, FRAME_MAX, &r);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE(r.rcp_reset);
}

TEST_CASE("On a corrupt pattern, or a data or accept length above frame_max: valid false, nothing taken",
          "[radio_mux]")
{
    uint8_t in[RADIO_MUX_SPI_HDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    radio_mux_spi_result_t r;

    radio_mux_spi_check(in, 10, 32, FRAME_MAX, &r);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT16(0, r.rx_len);
    TEST_ASSERT_FALSE(r.tx_taken);
    rcp_hdr(false, 2048, FRAME_MAX + 1, in);
    radio_mux_spi_check(in, 0, FRAME_MAX, FRAME_MAX, &r);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT16(0, r.rx_len);
    rcp_hdr(false, FRAME_MAX + 1, 10, in);
    radio_mux_spi_check(in, 0, FRAME_MAX, FRAME_MAX, &r);
    TEST_ASSERT_FALSE(r.valid);
}

/* ---- radio_mux_spi_xfer_len ---- */

TEST_CASE("On a receive transaction (accept 2048): clocks 2112 bytes (header + 2048, whole cache lines)",
          "[radio_mux]")
{
    TEST_ASSERT_EQUAL(2112, radio_mux_spi_xfer_len(0, 2048));
}

TEST_CASE("On a short frame of ours (accept 32): clocks 64 bytes; on 100 bytes: 128", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(64, radio_mux_spi_xfer_len(10, 32));
    TEST_ASSERT_EQUAL(64, radio_mux_spi_xfer_len(59, 59));
    TEST_ASSERT_EQUAL(128, radio_mux_spi_xfer_len(60, 60));
    TEST_ASSERT_EQUAL(128, radio_mux_spi_xfer_len(100, 100));
}

/* ---- radio_mux_c6_present ---- */

TEST_CASE("On no probe yet: present, reason empty", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {0};
    char why[64] = "x";

    TEST_ASSERT_TRUE(radio_mux_c6_present(&p, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
}

TEST_CASE("On an RCP installed: present", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_TRUE(radio_mux_c6_present(&p, NULL, 0));
}

TEST_CASE("On a silent updater without a failed recovery: present (the installed firmware is tried)", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.updater_reachable = false};
    char why[64];

    TEST_ASSERT_TRUE(radio_mux_c6_present(&p, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
}

TEST_CASE("On the factory ESP-Hosted firmware: unavailable, the reason names it", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.esp_hosted = true};
    char why[64];

    TEST_ASSERT_FALSE(radio_mux_c6_present(&p, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the C6 runs its factory ESP-Hosted firmware", why);
}

TEST_CASE("On a failed ROM recovery with a silent updater: unavailable, the reason names it", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.rom_recovery_failed = true};
    char why[64];

    TEST_ASSERT_FALSE(radio_mux_c6_present(&p, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the ROM recovery of the C6 failed", why);
}

TEST_CASE("On a failed ROM recovery with the updater answering and an RCP installed: present", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.rom_recovery_failed = true, .updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_TRUE(radio_mux_c6_present(&p, NULL, 0));
}

TEST_CASE("On the updater answering with no RCP installed: unavailable, the reason names it", "[radio_mux]")
{
    const radio_mux_c6_probe_t p = {.updater_reachable = true, .installed_valid = false};
    char why[64];

    TEST_ASSERT_FALSE(radio_mux_c6_present(&p, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("no RCP firmware on the C6", why);
}

/* ---- radio_mux_c6_job_refused ---- */

TEST_CASE("On rcp-proxy attached with a connected peer: the job is refused", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_c6_job_refused(RADIO_MUX_MODE_RCP_PROXY, true, true));
}

TEST_CASE("On rcp-proxy without a peer, otbr, off, or a detached rcp-proxy: the job is allowed", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_c6_job_refused(RADIO_MUX_MODE_RCP_PROXY, true, false));
    TEST_ASSERT_FALSE(radio_mux_c6_job_refused(RADIO_MUX_MODE_OTBR, true, true));
    TEST_ASSERT_FALSE(radio_mux_c6_job_refused(RADIO_MUX_MODE_OFF, false, false));
    TEST_ASSERT_FALSE(radio_mux_c6_job_refused(RADIO_MUX_MODE_RCP_PROXY, false, true));
}

/* ---- radio_mux_spi_gap_wait_us ---- */

#define GAP_US 100

TEST_CASE("On no transaction yet: may clock now (wait 0)", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_spi_gap_wait_us(5, -1, GAP_US));
}

TEST_CASE("On the gap passed, or exactly reached: may clock now (wait 0)", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_spi_gap_wait_us(1000 + GAP_US, 1000, GAP_US));
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_spi_gap_wait_us(50000, 1000, GAP_US));
}

TEST_CASE("On a transaction that just ended: waits the rest of the gap", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(GAP_US, radio_mux_spi_gap_wait_us(1000, 1000, GAP_US));
    TEST_ASSERT_EQUAL_UINT32(GAP_US - 30, radio_mux_spi_gap_wait_us(1030, 1000, GAP_US));
    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_spi_gap_wait_us(1000 + GAP_US - 1, 1000, GAP_US));
}

TEST_CASE("On a clock that went backwards: waits the whole gap", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(GAP_US, radio_mux_spi_gap_wait_us(900, 1000, GAP_US));
}
