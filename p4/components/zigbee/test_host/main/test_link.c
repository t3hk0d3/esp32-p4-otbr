#include <stdbool.h>
#include <stdint.h>
#include "unity.h"
#include "zigbee_link.h"

static const uint8_t FRAME[4] = {1, 2, 3, 4};

static void fill_queue(zigbee_link_t *link, size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        TEST_ASSERT_TRUE(zigbee_link_host_send(link, 10));
    }
}

TEST_CASE("queue depth: the link is built for 32 frames", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, ZIGBEE_LINK_QUEUE_DEPTH);
    TEST_ASSERT_EQUAL(32, ZIGBEE_LINK_QUEUE_DEPTH);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 32);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
}

TEST_CASE("init: no client, nothing queued, nothing to signal", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 4);
    TEST_ASSERT_EQUAL(-1, zigbee_link_client(&link));
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
    TEST_ASSERT_FALSE(zigbee_link_drained(&link));
}

/* ---- host send ---- */

TEST_CASE("host send with a client and room: returns true and takes a queue slot", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 4);
    TEST_ASSERT_TRUE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_TRUE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
}

TEST_CASE("host send with a full queue: returns false", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 3);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 3);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
}

TEST_CASE("host send with no client: returns true and takes no queue slot", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    fill_queue(&link, 10);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
}

TEST_CASE("host send after the client left: drops the frame and returns true", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 4);
    TEST_ASSERT_TRUE(zigbee_link_disconnect(&link, 4));
    fill_queue(&link, 5);
}

/* ---- drained (Review Focus 5) ---- */

TEST_CASE("backpressure: a full queue refuses and signals ready once drained", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 3);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 3);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE_MESSAGE(zigbee_link_drained(&link), "still full");
    zigbee_link_sent(&link);
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_drained(&link), "room again");
    TEST_ASSERT_FALSE_MESSAGE(zigbee_link_drained(&link), "only once");
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_host_send(&link, 10), "the retry fits");
}

TEST_CASE("backpressure: several refusals give one ready signal", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    zigbee_link_sent(&link);
    zigbee_link_sent(&link);
    TEST_ASSERT_TRUE(zigbee_link_drained(&link));
    TEST_ASSERT_FALSE(zigbee_link_drained(&link));
}

TEST_CASE("backpressure: with no refusal, drained stays false", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 3);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 3);
    zigbee_link_sent(&link);
    TEST_ASSERT_FALSE(zigbee_link_drained(&link));
}

TEST_CASE("backpressure: refused again after a retry, a second ready signal follows", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 1);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 1);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    zigbee_link_sent(&link);
    TEST_ASSERT_TRUE(zigbee_link_drained(&link));
    fill_queue(&link, 1);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_FALSE(zigbee_link_drained(&link));
    zigbee_link_sent(&link);
    TEST_ASSERT_TRUE(zigbee_link_drained(&link));
}

TEST_CASE("backpressure: a new client takes over a full queue and frames resume once it drains", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 4);
    fill_queue(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
    TEST_ASSERT_EQUAL(4, zigbee_link_connect(&link, 6));
    TEST_ASSERT_FALSE_MESSAGE(zigbee_link_drained(&link), "the old client's frames still hold the slots");
    zigbee_link_sent(&link); // the sender discards the old client's frames
    zigbee_link_sent(&link);
    TEST_ASSERT_TRUE(zigbee_link_drained(&link));
    TEST_ASSERT_TRUE(zigbee_link_host_send(&link, 10));
}

TEST_CASE("sent with an empty queue: the count stays at 0", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 4);
    zigbee_link_sent(&link);
    zigbee_link_sent(&link);
    fill_queue(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_host_send(&link, 10));
}

/* ---- the kept hello ---- */

TEST_CASE("first outbound frame: is kept as the hello", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    TEST_ASSERT_TRUE(zigbee_link_take_hello(&link, FRAME, sizeof(FRAME)));
}

TEST_CASE("later outbound frames: are not kept as the hello", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    TEST_ASSERT_FALSE(zigbee_link_take_hello(&link, FRAME, sizeof(FRAME)));
    TEST_ASSERT_FALSE(zigbee_link_take_hello(&link, FRAME, sizeof(FRAME)));
}

TEST_CASE("an empty or missing frame: is not kept and does not use up the hello", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_take_hello(&link, FRAME, 0));
    TEST_ASSERT_FALSE(zigbee_link_take_hello(&link, NULL, 4));
    TEST_ASSERT_TRUE(zigbee_link_take_hello(&link, FRAME, sizeof(FRAME)));
}

TEST_CASE("an oversized frame: is not kept as the hello", "[zigbee]")
{
    zigbee_link_t link;
    static uint8_t big[ZIGBEE_LINK_MAX_FRAME + 1];

    zigbee_link_init(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_take_hello(&link, big, sizeof(big)));
    TEST_ASSERT_TRUE(zigbee_link_take_hello(&link, big, ZIGBEE_LINK_MAX_FRAME));
}

/* ---- client connect ---- */

TEST_CASE("client connect with no previous client: returns -1 and makes this the client", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    TEST_ASSERT_EQUAL(-1, zigbee_link_connect(&link, 7));
    TEST_ASSERT_EQUAL(7, zigbee_link_client(&link));
}

TEST_CASE("client connect with a kept hello: the hello is due first, once", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_take_send_hello(&link));
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
}

TEST_CASE("client connect before the stack sent its first frame: no hello is due", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_take_hello(&link, FRAME, sizeof(FRAME)), "the first frame becomes the hello");
    TEST_ASSERT_FALSE_MESSAGE(zigbee_link_take_send_hello(&link), "and goes out as an ordinary frame");
}

TEST_CASE("client connect with another client connected: replaces it and returns its fd", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_EQUAL(7, zigbee_link_connect(&link, 9));
    TEST_ASSERT_EQUAL(9, zigbee_link_client(&link));
}

TEST_CASE("client connect with another client connected: the hello is due again for the new client", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_take_send_hello(&link));
    zigbee_link_connect(&link, 9);
    TEST_ASSERT_TRUE(zigbee_link_take_send_hello(&link));
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
}

TEST_CASE("client connect replacing a client before its hello went out: one hello for the new client", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    zigbee_link_connect(&link, 9);
    TEST_ASSERT_TRUE(zigbee_link_take_send_hello(&link));
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
}

TEST_CASE("client connect with the fd of the current client (reused after a close): returns -1 and sends a hello", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    zigbee_link_take_send_hello(&link);
    TEST_ASSERT_EQUAL_MESSAGE(-1, zigbee_link_connect(&link, 7), "the caller must not close the new socket");
    TEST_ASSERT_EQUAL(7, zigbee_link_client(&link));
    TEST_ASSERT_TRUE(zigbee_link_take_send_hello(&link));
}

/* ---- client leaves ---- */

TEST_CASE("client disconnect of the current client: returns true and no client remains", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_disconnect(&link, 7));
    TEST_ASSERT_EQUAL(-1, zigbee_link_client(&link));
}

TEST_CASE("client disconnect of a replaced client: returns false and the new client stays", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    zigbee_link_connect(&link, 9);
    TEST_ASSERT_FALSE(zigbee_link_disconnect(&link, 7));
    TEST_ASSERT_EQUAL(9, zigbee_link_client(&link));
}

TEST_CASE("client disconnect with no client: returns false", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_disconnect(&link, 7));
    TEST_ASSERT_FALSE(zigbee_link_disconnect(&link, -1));
}

TEST_CASE("client disconnect before its hello went out: no hello is due", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    zigbee_link_disconnect(&link, 7);
    TEST_ASSERT_FALSE(zigbee_link_take_send_hello(&link));
}

/* ---- inbound frames ---- */

TEST_CASE("inbound binary frame from the client: accepted", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_accept_inbound(&link, 7, 100, true));
    TEST_ASSERT_FALSE(zigbee_link_must_close(100));
}

TEST_CASE("inbound frame of exactly 2048 bytes: accepted and the socket stays open", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_accept_inbound(&link, 7, 2048, true));
    TEST_ASSERT_FALSE(zigbee_link_must_close(2048));
}

TEST_CASE("inbound frame over 2048 bytes: dropped and the socket must be closed", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_FALSE(zigbee_link_accept_inbound(&link, 7, 2049, true));
    TEST_ASSERT_TRUE(zigbee_link_must_close(2049));
}

TEST_CASE("inbound frame that is not binary: ignored and the socket stays open", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_FALSE(zigbee_link_accept_inbound(&link, 7, 100, false));
    TEST_ASSERT_FALSE(zigbee_link_must_close(100));
}

TEST_CASE("inbound empty frame: ignored", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_FALSE(zigbee_link_accept_inbound(&link, 7, 0, true));
}

TEST_CASE("inbound frame with no client: dropped", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    TEST_ASSERT_FALSE(zigbee_link_accept_inbound(&link, 7, 100, true));
    TEST_ASSERT_FALSE(zigbee_link_accept_inbound(&link, -1, 100, true));
}

TEST_CASE("takeover: late frames from the old client are dropped", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 2);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    TEST_ASSERT_TRUE(zigbee_link_accept_inbound(&link, 7, 100, true));
    TEST_ASSERT_EQUAL_MESSAGE(7, zigbee_link_connect(&link, 9), "the old socket is handed back to be closed");
    TEST_ASSERT_FALSE_MESSAGE(zigbee_link_accept_inbound(&link, 7, 100, true), "late frame of the old client");
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_accept_inbound(&link, 9, 100, true), "frame of the new client");
}

TEST_CASE("takeover: the newest client gets the hello first and then every later frame", "[zigbee]")
{
    zigbee_link_t link;

    zigbee_link_init(&link, 4);
    zigbee_link_take_hello(&link, FRAME, sizeof(FRAME));
    zigbee_link_connect(&link, 7);
    zigbee_link_take_send_hello(&link);
    TEST_ASSERT_TRUE(zigbee_link_host_send(&link, 10));
    zigbee_link_connect(&link, 9);
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_take_send_hello(&link), "hello first");
    TEST_ASSERT_TRUE_MESSAGE(zigbee_link_host_send(&link, 10), "then every later frame");
    TEST_ASSERT_EQUAL(9, zigbee_link_client(&link));
}
