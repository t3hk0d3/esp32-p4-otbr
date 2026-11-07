#include <string.h>
#include "radio_mux_pool.h"
#include "unity.h"
#include "unity_test_runner.h"

#define SLOT_SIZE 16
#define SLOTS 4

static uint8_t slots[RADIO_MUX_POOL_MAX_SLOTS * SLOT_SIZE];

static void make_pool(radio_mux_pool_t *p, size_t n)
{
    radio_mux_pool_init(p, slots, SLOT_SIZE, n);
}

TEST_CASE("On a free slot: returns it", "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t *a, *b;

    make_pool(&p, SLOTS);
    a = radio_mux_pool_take(&p);
    b = radio_mux_pool_take(&p);

    TEST_ASSERT_EQUAL_PTR(slots, a);
    TEST_ASSERT_EQUAL_PTR(slots + SLOT_SIZE, b);
}

TEST_CASE("On a free slot: in_use + 1", "[radio_mux]")
{
    radio_mux_pool_t p;

    make_pool(&p, SLOTS);
    TEST_ASSERT_EQUAL_UINT32(0, p.in_use);
    radio_mux_pool_take(&p);
    TEST_ASSERT_EQUAL_UINT32(1, p.in_use);
    radio_mux_pool_take(&p);
    TEST_ASSERT_EQUAL_UINT32(2, p.in_use);
    TEST_ASSERT_EQUAL_UINT32(0, p.drops);
}

TEST_CASE("On a full pool: returns NULL", "[radio_mux]")
{
    radio_mux_pool_t p;

    make_pool(&p, SLOTS);
    for (size_t i = 0; i < SLOTS; i++) {
        TEST_ASSERT_NOT_NULL(radio_mux_pool_take(&p));
    }

    TEST_ASSERT_NULL(radio_mux_pool_take(&p));
    TEST_ASSERT_EQUAL_UINT32(SLOTS, p.in_use);
}

TEST_CASE("On a full pool: drops + 1", "[radio_mux]")
{
    radio_mux_pool_t p;

    make_pool(&p, SLOTS);
    for (size_t i = 0; i < SLOTS; i++) {
        radio_mux_pool_take(&p);
    }

    radio_mux_pool_take(&p);
    radio_mux_pool_take(&p);

    TEST_ASSERT_EQUAL_UINT32(2, p.drops);
}

TEST_CASE("On a pool of 32 slots: hands out every slot once", "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t *got[RADIO_MUX_POOL_MAX_SLOTS];

    make_pool(&p, RADIO_MUX_POOL_MAX_SLOTS);
    for (size_t i = 0; i < RADIO_MUX_POOL_MAX_SLOTS; i++) {
        got[i] = radio_mux_pool_take(&p);
        TEST_ASSERT_EQUAL_PTR(slots + i * SLOT_SIZE, got[i]);
    }

    TEST_ASSERT_NULL(radio_mux_pool_take(&p));
    TEST_ASSERT_EQUAL_UINT32(0, p.free_mask);
}

TEST_CASE("On a pool larger than 32 slots: uses 32", "[radio_mux]")
{
    radio_mux_pool_t p;

    make_pool(&p, RADIO_MUX_POOL_MAX_SLOTS + 8);

    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_POOL_MAX_SLOTS, p.n);
}

TEST_CASE("On putting a slot back: makes it available again", "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t *first;

    make_pool(&p, SLOTS);
    first = radio_mux_pool_take(&p);
    for (size_t i = 1; i < SLOTS; i++) {
        radio_mux_pool_take(&p);
    }
    TEST_ASSERT_NULL(radio_mux_pool_take(&p));

    radio_mux_pool_put(&p, first);

    TEST_ASSERT_EQUAL_UINT32(SLOTS - 1, p.in_use);
    TEST_ASSERT_EQUAL_PTR(first, radio_mux_pool_take(&p));
    TEST_ASSERT_EQUAL_UINT32(SLOTS, p.in_use);
}

TEST_CASE("On putting a slot twice: leaves the accounting unchanged", "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t *a;
    uint32_t mask;

    make_pool(&p, SLOTS);
    a = radio_mux_pool_take(&p);
    radio_mux_pool_take(&p);
    radio_mux_pool_put(&p, a);
    mask = p.free_mask;

    radio_mux_pool_put(&p, a);

    TEST_ASSERT_EQUAL_UINT32(1, p.in_use);
    TEST_ASSERT_EQUAL_UINT32(mask, p.free_mask);
}

TEST_CASE("On putting a slot twice: bad_puts + 1", "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t *a;

    make_pool(&p, SLOTS);
    a = radio_mux_pool_take(&p);
    radio_mux_pool_put(&p, a);
    TEST_ASSERT_EQUAL_UINT32(0, p.bad_puts);

    radio_mux_pool_put(&p, a);

    TEST_ASSERT_EQUAL_UINT32(1, p.bad_puts);
}

TEST_CASE("On putting a pointer that is no slot start: leaves the accounting unchanged and counts bad_puts",
          "[radio_mux]")
{
    radio_mux_pool_t p;
    uint8_t other[SLOT_SIZE];
    uint8_t *bad[] = {NULL, other, slots + 1, slots + SLOT_SIZE + 3, slots + SLOTS * SLOT_SIZE};
    uint32_t mask;

    make_pool(&p, SLOTS);
    radio_mux_pool_take(&p);
    radio_mux_pool_take(&p);
    mask = p.free_mask;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_pool_put(&p, bad[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(2, p.in_use);
    TEST_ASSERT_EQUAL_UINT32(mask, p.free_mask);
    TEST_ASSERT_EQUAL_UINT32(sizeof(bad) / sizeof(bad[0]), p.bad_puts);
}
