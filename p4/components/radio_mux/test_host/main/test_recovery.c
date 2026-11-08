// USB recovery decisions: the retried close, the reopen after a transfer error, which resets are carried out.
#include "radio_mux_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define NOW 5000

/* ---- radio_mux_close_retry ---- */

TEST_CASE("On no failed close: nothing is due", "[radio_mux]")
{
    radio_mux_close_retry_t r = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_IDLE, radio_mux_close_retry_due(&r, NOW));
}

TEST_CASE("On a failed close: the first retry waits RADIO_MUX_CLOSE_RETRY_MS", "[radio_mux]")
{
    radio_mux_close_retry_t r;

    radio_mux_close_retry_start(&r, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_WAIT, radio_mux_close_retry_due(&r, NOW + RADIO_MUX_CLOSE_RETRY_MS - 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_TRY, radio_mux_close_retry_due(&r, NOW + RADIO_MUX_CLOSE_RETRY_MS));
}

TEST_CASE("On a retry that closes: nothing is pending any more", "[radio_mux]")
{
    radio_mux_close_retry_t r;

    radio_mux_close_retry_start(&r, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_DONE, radio_mux_close_retry_result(&r, NOW + 300, true));
    TEST_ASSERT_FALSE(r.pending);
    TEST_ASSERT_FALSE(r.gave_up);
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_IDLE, radio_mux_close_retry_due(&r, NOW + 10000));
}

TEST_CASE("On a retry that fails: the next one is due RADIO_MUX_CLOSE_RETRY_MS later", "[radio_mux]")
{
    radio_mux_close_retry_t r;

    radio_mux_close_retry_start(&r, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_WAIT, radio_mux_close_retry_result(&r, NOW + 300, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_WAIT, radio_mux_close_retry_due(&r, NOW + 300 + RADIO_MUX_CLOSE_RETRY_MS - 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_TRY, radio_mux_close_retry_due(&r, NOW + 300 + RADIO_MUX_CLOSE_RETRY_MS));
}

TEST_CASE("On the last retry failing: gives up once, then nothing is due", "[radio_mux]")
{
    radio_mux_close_retry_t r;
    int64_t t = NOW;

    radio_mux_close_retry_start(&r, t);
    for (int i = 1; i < RADIO_MUX_CLOSE_RETRIES; i++) {
        t += RADIO_MUX_CLOSE_RETRY_MS;
        TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_WAIT, radio_mux_close_retry_result(&r, t, false));
    }
    t += RADIO_MUX_CLOSE_RETRY_MS;
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_GAVE_UP, radio_mux_close_retry_result(&r, t, false));
    TEST_ASSERT_TRUE(r.gave_up);
    TEST_ASSERT_FALSE(r.pending);
    TEST_ASSERT_EQUAL(RADIO_MUX_CLOSE_IDLE, radio_mux_close_retry_due(&r, t + 100000));
}

TEST_CASE("On the retries: they span a few seconds", "[radio_mux]")
{
    const int total_ms = RADIO_MUX_CLOSE_RETRIES * RADIO_MUX_CLOSE_RETRY_MS;

    TEST_ASSERT_TRUE(total_ms >= 2000 && total_ms <= 5000);
}

/* ---- radio_mux_usb_error_due ---- */

TEST_CASE("On no flagged USB error: no reopen", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_usb_error_due(false, 0, NOW));
}

TEST_CASE("On a first flagged USB error: reopens at once", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_usb_error_due(true, 0, NOW));
}

TEST_CASE("On a flagged USB error within a second of the last reopen: waits", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_usb_error_due(true, NOW, NOW + RADIO_MUX_USB_ERROR_REOPEN_US - 1));
}

TEST_CASE("On a flagged USB error a second after the last reopen: reopens", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_usb_error_due(true, NOW, NOW + RADIO_MUX_USB_ERROR_REOPEN_US));
}

TEST_CASE("On a clock that went backwards: the flagged error reopens", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_usb_error_due(true, NOW, NOW - 1));
}

/* ---- radio_mux_reset_applies ---- */

TEST_CASE("On a reset of an attached channel through its current handle: carried out", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_reset_applies(RADIO_MUX_AS_ATTACHED, false, true, true));
}

TEST_CASE("On a reset of a leased channel (a move's target) through the lease's handle: carried out", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_reset_applies(RADIO_MUX_AS_IDLE, true, true, true));
}

TEST_CASE("On a reset through an earlier attach's or lease's handle: ignored", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_ATTACHED, false, true, false));
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_IDLE, true, true, false));
}

TEST_CASE("On a reset of a channel neither attached nor leased: ignored", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_IDLE, false, true, true));
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_PROBING, false, true, true));
}

TEST_CASE("On a reset of a channel no longer on its transport: ignored", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_ATTACHED, false, false, true));
    TEST_ASSERT_FALSE(radio_mux_reset_applies(RADIO_MUX_AS_IDLE, true, false, true));
}
