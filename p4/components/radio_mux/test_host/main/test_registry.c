// Channel registry decisions: which slot a new device takes, one otbr channel at load, the firmware record kept.
#include "radio_mux_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define N_SLOTS 7

static radio_mux_slot_view_t slots[N_SLOTS];

// Every slot used by an unplugged, remembered channel in mode off, slot i last seen at 100 + i.
static void full_list(void)
{
    for (size_t i = 0; i < N_SLOTS; i++) {
        slots[i] = (radio_mux_slot_view_t) {
            .used = true, .present = false, .on_port = false, .remembered = true, .mode = RADIO_MUX_MODE_OFF,
            .last_seen = 100 + (int64_t)i,
        };
    }
}

/* ---- radio_mux_evict_pick ---- */

TEST_CASE("On a free slot: the new device takes the first one", "[radio_mux]")
{
    full_list();
    slots[4].used = false;
    slots[5].used = false;
    TEST_ASSERT_EQUAL_UINT32(4, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On a full list: the unplugged off channel seen longest ago is picked", "[radio_mux]")
{
    full_list();
    slots[3].last_seen = 5;
    TEST_ASSERT_EQUAL_UINT32(3, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On a full list: a channel stored in otbr is never picked", "[radio_mux]")
{
    full_list();
    slots[0].mode = RADIO_MUX_MODE_OTBR; // the oldest
    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On a full list: channels stored in rcp-proxy or ziggurat are never picked", "[radio_mux]")
{
    full_list();
    slots[0].mode = RADIO_MUX_MODE_RCP_PROXY;
    slots[1].mode = RADIO_MUX_MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL_UINT32(2, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On a full list: a present channel or the transport's own is never picked", "[radio_mux]")
{
    full_list();
    slots[0].present = true;
    slots[1].on_port = true;
    TEST_ASSERT_EQUAL_UINT32(2, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On a full list with nothing in mode off: no slot (the device is not listed)", "[radio_mux]")
{
    full_list();
    for (size_t i = 0; i < N_SLOTS; i++) {
        slots[i].mode = i == 0 ? RADIO_MUX_MODE_OTBR : RADIO_MUX_MODE_RCP_PROXY;
    }
    TEST_ASSERT_EQUAL_UINT32(N_SLOTS, radio_mux_evict_pick(slots, N_SLOTS, false));
}

TEST_CASE("On an unsupported device and a full list of remembered channels: no slot", "[radio_mux]")
{
    full_list();
    TEST_ASSERT_EQUAL_UINT32(N_SLOTS, radio_mux_evict_pick(slots, N_SLOTS, true));
}

TEST_CASE("On an unsupported device: a free slot is taken", "[radio_mux]")
{
    full_list();
    slots[6].used = false;
    TEST_ASSERT_EQUAL_UINT32(6, radio_mux_evict_pick(slots, N_SLOTS, true));
}

TEST_CASE("On an unsupported device: an unplugged slot that is not remembered may be picked", "[radio_mux]")
{
    full_list();
    slots[2].remembered = false;
    TEST_ASSERT_EQUAL_UINT32(2, radio_mux_evict_pick(slots, N_SLOTS, true));
}

/* ---- radio_mux_otbr_dedupe ---- */

TEST_CASE("On one otbr channel: nothing changes", "[radio_mux]")
{
    radio_mux_mode_t m[3] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_RCP_PROXY};
    bool changed[3] = {true, true, true};

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_otbr_dedupe(m, changed, 3));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[1]);
    TEST_ASSERT_FALSE(changed[0] || changed[1] || changed[2]);
}

TEST_CASE("On the C6 and a dongle both stored otbr: the C6 (slot 0) keeps it, the dongle becomes off", "[radio_mux]")
{
    radio_mux_mode_t m[3] = {RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OFF};
    bool changed[3];

    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_otbr_dedupe(m, changed, 3));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[0]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[1]);
    TEST_ASSERT_FALSE(changed[0]);
    TEST_ASSERT_TRUE(changed[1]);
    TEST_ASSERT_FALSE(changed[2]);
}

TEST_CASE("On two dongles stored otbr: the first in slot order keeps it", "[radio_mux]")
{
    radio_mux_mode_t m[4] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OTBR};
    bool changed[4];

    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_otbr_dedupe(m, changed, 4));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[2]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[3]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY, m[1]);
    TEST_ASSERT_TRUE(changed[3]);
}

TEST_CASE("On no otbr channel: nothing changes", "[radio_mux]")
{
    radio_mux_mode_t m[2] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OFF};
    bool changed[2];

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_otbr_dedupe(m, changed, 2));
}

/* ---- radio_mux_detect_keeps_record ---- */

TEST_CASE("On a detection that found nothing over a known firmware: the record is kept", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_detect_keeps_record(RADIO_MUX_FW_SPINEL, RADIO_MUX_FW_UNKNOWN));
    TEST_ASSERT_TRUE(radio_mux_detect_keeps_record(RADIO_MUX_FW_EZSP, RADIO_MUX_FW_UNKNOWN));
}

TEST_CASE("On a detection that found nothing over no record: unknown is recorded", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_detect_keeps_record(RADIO_MUX_FW_UNKNOWN, RADIO_MUX_FW_UNKNOWN));
}

TEST_CASE("On a detection that found a firmware: it replaces the record", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_detect_keeps_record(RADIO_MUX_FW_SPINEL, RADIO_MUX_FW_ZNP));
    TEST_ASSERT_FALSE(radio_mux_detect_keeps_record(RADIO_MUX_FW_UNKNOWN, RADIO_MUX_FW_SPINEL));
}
