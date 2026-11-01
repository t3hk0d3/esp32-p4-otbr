// UART1 ownership between the C6 log receiver and the flasher (spec 5.10), including the stale-request cases.
#include "unity.h"
#include "uart_handoff.h"

static uart_handoff_t h;

/* One full receiver step: poll and, on RELEASE, delete the driver. Returns the action. */
static handoff_action_t receiver_step(void)
{
    const handoff_action_t a = uart_handoff_poll(&h);

    if (a == HANDOFF_ACTION_RELEASE) {
        uart_handoff_released(&h);
    }
    return a;
}

TEST_CASE("On a pause the receiver answers: the UART is free after it released the driver and the receiver does not own it", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    TEST_ASSERT_TRUE(uart_handoff_request_pause(&h));
    TEST_ASSERT_FALSE(uart_handoff_is_paused(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_RELEASE, receiver_step());
    TEST_ASSERT_TRUE(uart_handoff_is_paused(&h));
    TEST_ASSERT_FALSE(uart_handoff_receiver_owns_uart(&h));
}

TEST_CASE("On a pause the receiver did not see yet: the UART is not reported free", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);
    TEST_ASSERT_FALSE(uart_handoff_is_paused(&h));
}

TEST_CASE("On resume after a pause: the receiver installs the driver again exactly once", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);
    receiver_step();
    uart_handoff_request_resume(&h);
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_REINSTALL, receiver_step());
    TEST_ASSERT_TRUE(uart_handoff_receiver_owns_uart(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_NONE, receiver_step());
}

TEST_CASE("On a pause that timed out and a resume: the request is cancelled and the receiver does nothing", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);      /* the receiver never ran */
    uart_handoff_request_resume(&h);     /* the caller gave up */
    TEST_ASSERT_TRUE(uart_handoff_receiver_owns_uart(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_NONE, receiver_step());
}

TEST_CASE("On pause timeout, resume, then pause again: the receiver does not take the UART back while the flasher owns it", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);
    uart_handoff_request_resume(&h);     /* stale token scenario of the first version */
    TEST_ASSERT_TRUE(uart_handoff_request_pause(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_RELEASE, receiver_step());
    TEST_ASSERT_TRUE(uart_handoff_is_paused(&h));
    for (int i = 0; i < 5; i++) {        /* the flasher works: the receiver stays away */
        TEST_ASSERT_EQUAL(HANDOFF_ACTION_NONE, receiver_step());
        TEST_ASSERT_FALSE(uart_handoff_receiver_owns_uart(&h));
    }
}

TEST_CASE("On a resume that arrives while the receiver is releasing: the receiver releases, then reinstalls", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_RELEASE, uart_handoff_poll(&h));
    uart_handoff_request_resume(&h);     /* before released() */
    uart_handoff_released(&h);
    TEST_ASSERT_FALSE(uart_handoff_is_paused(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_REINSTALL, receiver_step());
    TEST_ASSERT_TRUE(uart_handoff_receiver_owns_uart(&h));
}

TEST_CASE("On a second pause while a resume is still pending: it is refused", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_pause(&h);
    receiver_step();
    uart_handoff_request_resume(&h);
    TEST_ASSERT_FALSE(uart_handoff_request_pause(&h));
}

TEST_CASE("On a resume without a pause: nothing changes", "[rcp_sync][handoff]")
{
    uart_handoff_init(&h);
    uart_handoff_request_resume(&h);
    TEST_ASSERT_TRUE(uart_handoff_receiver_owns_uart(&h));
    TEST_ASSERT_EQUAL(HANDOFF_ACTION_NONE, receiver_step());
}
