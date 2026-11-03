#include <stdbool.h>
#include <stdint.h>
#include "unity.h"
#include "br_core_leader.h"

/* running, attached, is_leader, router_eligible, own_weight, leader_weight */

TEST_CASE("-16: valid", "[br_core]")
{
    TEST_ASSERT_TRUE(br_core_leader_adjustment_valid(-16));
}

TEST_CASE("0: valid", "[br_core]")
{
    TEST_ASSERT_TRUE(br_core_leader_adjustment_valid(0));
}

TEST_CASE("16: valid", "[br_core]")
{
    TEST_ASSERT_TRUE(br_core_leader_adjustment_valid(16));
}

TEST_CASE("-17: invalid", "[br_core]")
{
    TEST_ASSERT_FALSE(br_core_leader_adjustment_valid(-17));
}

TEST_CASE("17: invalid", "[br_core]")
{
    TEST_ASSERT_FALSE(br_core_leader_adjustment_valid(17));
}

TEST_CASE("Not running: NOT_RUNNING", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_NOT_RUNNING, br_core_leader_check(false, true, false, true, 90, 65));
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_NOT_RUNNING, br_core_leader_check(false, false, true, false, 10, 65));
}

TEST_CASE("Running, not attached: NOT_ATTACHED", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_NOT_ATTACHED, br_core_leader_check(true, false, false, true, 90, 65));
}

TEST_CASE("Attached, this board is leader: ALREADY", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_ALREADY, br_core_leader_check(true, true, true, true, 90, 65));
}

TEST_CASE("Attached, not router eligible: NOT_ELIGIBLE", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_NOT_ELIGIBLE, br_core_leader_check(true, true, false, false, 90, 65));
}

TEST_CASE("Own 65, leader 65: WEIGHT_NOT_HIGHER", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_WEIGHT_NOT_HIGHER, br_core_leader_check(true, true, false, true, 65, 65));
}

TEST_CASE("Own 64, leader 65: WEIGHT_NOT_HIGHER", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_WEIGHT_NOT_HIGHER, br_core_leader_check(true, true, false, true, 64, 65));
}

TEST_CASE("Own 66, leader 65, eligible, attached, not leader: OK", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_OK, br_core_leader_check(true, true, false, true, 66, 65));
}

TEST_CASE("Not attached and not eligible together: NOT_ATTACHED", "[br_core]")
{
    TEST_ASSERT_EQUAL(BR_CORE_LEADER_NOT_ATTACHED, br_core_leader_check(true, false, false, false, 90, 65));
}
