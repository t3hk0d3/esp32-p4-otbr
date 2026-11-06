#include <stdbool.h>
#include <stdint.h>

#include "unity.h"
#include "zigbee_kick.h"

/*
 * A model of the zigbee task's queue and mailbox around zigbee_kick_t, as zigbee.c uses it: producers add mailbox
 * work and claim a KICK; the task dequeues KICKs (zigbee_kick_taken, then it reads the whole mailbox) and also reads
 * the mailbox after every other item it takes.
 */
typedef struct {
    zigbee_kick_t kick;
    int queued_kicks;  // KICKs in the queue
    int max_queued;
    int mailbox;       // work items not read yet
    int queue_cap_left; // room for KICKs (the rest of the queue is the mux's)
} model_t;

static void model_init(model_t *m, int cap)
{
    zigbee_kick_init(&m->kick);
    m->queued_kicks = 0;
    m->max_queued = 0;
    m->mailbox = 0;
    m->queue_cap_left = cap;
}

static void produce(model_t *m)
{
    m->mailbox++;
    if (zigbee_kick_claim(&m->kick)) {
        if (m->queue_cap_left > 0) {
            m->queue_cap_left--;
            m->queued_kicks++;
            if (m->queued_kicks > m->max_queued) {
                m->max_queued = m->queued_kicks;
            }
        } else {
            zigbee_kick_unclaim(&m->kick);
        }
    }
}

static bool take_kick(model_t *m)
{
    if (m->queued_kicks == 0) {
        return false;
    }
    m->queued_kicks--;
    m->queue_cap_left++;
    zigbee_kick_taken(&m->kick);
    m->mailbox = 0; // the task reads the mailbox after taking the KICK
    return true;
}

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

TEST_CASE("kick burst: many wakes before a dequeue queue one KICK", "[zigbee]")
{
    model_t m;

    model_init(&m, 8);
    for (int i = 0; i < 1000; i++) {
        produce(&m);
    }
    TEST_ASSERT_EQUAL_INT(1, m.queued_kicks);
    TEST_ASSERT_TRUE(take_kick(&m));
    TEST_ASSERT_EQUAL_INT(0, m.queued_kicks);
    TEST_ASSERT_EQUAL_INT(0, m.mailbox);
}

TEST_CASE("kick interleaved: wakes between dequeues never queue a second KICK", "[zigbee]")
{
    model_t m;
    uint32_t seed = 12345;

    model_init(&m, 8);
    for (int i = 0; i < 100000; i++) {
        if (lcg(&seed) % 3 != 0) {
            produce(&m);
        } else {
            take_kick(&m);
        }
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, m.queued_kicks);
    }
    TEST_ASSERT_EQUAL_INT(1, m.max_queued);
}

TEST_CASE("kick interleaved: work posted after a dequeue always has a KICK queued", "[zigbee]")
{
    model_t m;
    uint32_t seed = 777;

    model_init(&m, 8);
    for (int i = 0; i < 100000; i++) {
        if (lcg(&seed) % 2) {
            produce(&m);
        } else {
            take_kick(&m);
        }
        // Unread work is never stranded: a KICK for it is queued.
        if (m.mailbox > 0) {
            TEST_ASSERT_EQUAL_INT(1, m.queued_kicks);
        }
    }
}

TEST_CASE("kick taken: the next wake posts a new KICK", "[zigbee]")
{
    zigbee_kick_t k;

    zigbee_kick_init(&k);
    TEST_ASSERT_TRUE(zigbee_kick_claim(&k));
    TEST_ASSERT_FALSE(zigbee_kick_claim(&k));
    zigbee_kick_taken(&k);
    TEST_ASSERT_TRUE(zigbee_kick_claim(&k));
}

TEST_CASE("kick post failed: the next wake tries again", "[zigbee]")
{
    zigbee_kick_t k;

    zigbee_kick_init(&k);
    TEST_ASSERT_TRUE(zigbee_kick_claim(&k));
    zigbee_kick_unclaim(&k);
    TEST_ASSERT_TRUE(zigbee_kick_claim(&k));
}

TEST_CASE("gone lost: the current attachment detached without its GONE is handled as gone", "[zigbee]")
{
    TEST_ASSERT_TRUE(zigbee_gone_lost(3, 3, 2));
}

TEST_CASE("gone lost: a GONE already handled for the attachment changes nothing", "[zigbee]")
{
    TEST_ASSERT_FALSE(zigbee_gone_lost(3, 3, 3));
}

TEST_CASE("gone lost: a detach of an older attachment changes nothing", "[zigbee]")
{
    TEST_ASSERT_FALSE(zigbee_gone_lost(2, 3, 1));
}

TEST_CASE("gone lost: a detach before the task handled its ATTACH waits for it", "[zigbee]")
{
    TEST_ASSERT_FALSE(zigbee_gone_lost(4, 3, 3));
    TEST_ASSERT_TRUE(zigbee_gone_lost(4, 4, 3)); // once the ATTACH is handled
}

TEST_CASE("gone lost: nothing attached yet is never gone", "[zigbee]")
{
    TEST_ASSERT_FALSE(zigbee_gone_lost(0, 0, 0));
}
