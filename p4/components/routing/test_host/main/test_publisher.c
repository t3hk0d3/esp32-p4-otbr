#include <string.h>
#include "unity.h"
#include "routing.h"
#include "publisher.h"
#include "fake_ops.h"

static const routing_prefix64_t CUSTOM = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}};
static const routing_prefix64_t CUSTOM2 = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x43, 0x01, 0x00}};
static const routing_prefix64_t ULA = {{0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01}};
static const routing_prefix64_t OTHER = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x99, 0x00, 0x00}};

static routing_publisher_t s_pub;

/* A fresh fake whose get_omr_prefix returns ULA, and a publisher started on it with the calls of init forgotten. */
static void start(void)
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    routing_publisher_init(&s_pub, &fake_ops);
    fake_ops_clear_calls();
}

static routing_settings_t settings(routing_method_t method, routing_omr_mode_t mode, const routing_prefix64_t *custom,
                                   bool announce_ula)
{
    routing_settings_t s;

    routing_settings_defaults(&s);
    s.method = method;
    s.omr_mode = mode;
    if (custom) {
        s.omr_prefix = *custom;
    }
    s.announce_ula = announce_ula;
    return s;
}

static void apply(routing_method_t method, routing_omr_mode_t mode, const routing_prefix64_t *custom,
                  bool announce_ula)
{
    routing_settings_t s = settings(method, mode, custom, announce_ula);
    routing_publisher_apply(&s_pub, &s);
}

static void assert_prefix(const routing_prefix64_t *expected, const routing_prefix64_t *actual)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->b, actual->b, sizeof(expected->b));
}

static bool call_has_prefix(const fake_call_t *c, const routing_prefix64_t *prefix)
{
    return c->has_prefix && memcmp(c->prefix.b, prefix->b, sizeof(prefix->b)) == 0;
}

/* --- init --- */

TEST_CASE("Border router started: records the self-generated ULA before applying a custom OMR config", "[routing]")
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    routing_publisher_init(&s_pub, &fake_ops);
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, false);

    int get = fake_ops_index(FAKE_GET_OMR_PREFIX, 0);
    int set = fake_ops_index(FAKE_SET_OMR_CONFIG, 0);
    TEST_ASSERT_TRUE(get >= 0);
    TEST_ASSERT_TRUE(set > get);
    routing_announce_input_t io = {0};
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_TRUE(io.ula_known);
    assert_prefix(&ULA, &io.ula);
}

TEST_CASE("Border router started: makes no call but get_omr_prefix", "[routing]")
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    routing_publisher_init(&s_pub, &fake_ops);

    TEST_ASSERT_EQUAL(1, g_fake.call_count);
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_GET_OMR_PREFIX));
}

TEST_CASE("ULA not readable at border-router up: leaves the ULA unknown", "[routing]")
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    g_fake.ret[FAKE_GET_OMR_PREFIX] = 13; /* OT_ERROR_INVALID_STATE */
    routing_publisher_init(&s_pub, &fake_ops);

    routing_announce_input_t io = {.ula_known = true, .ula_in_netdata = true};
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_FALSE(io.ula_known);
    TEST_ASSERT_FALSE(io.ula_in_netdata);
}

TEST_CASE("ULA not readable at border-router up: sync_default_route learns it later", "[routing]")
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    g_fake.ret[FAKE_GET_OMR_PREFIX] = 13;
    routing_publisher_init(&s_pub, &fake_ops);
    routing_publisher_sync_default_route(&s_pub);
    g_fake.ret[FAKE_GET_OMR_PREFIX] = 0;
    routing_publisher_sync_default_route(&s_pub);

    routing_announce_input_t io = {0};
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_TRUE(io.ula_known);
    assert_prefix(&ULA, &io.ula);
    TEST_ASSERT_EQUAL(3, fake_ops_count(FAKE_GET_OMR_PREFIX));
    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(3, fake_ops_count(FAKE_GET_OMR_PREFIX)); /* known now: not asked again */
}

TEST_CASE("ULA not readable at border-router up: sync_default_route adds the ULA entry once learned, in custom mode "
          "with the box ticked", "[routing]")
{
    fake_ops_reset();
    g_fake.omr_prefix = ULA;
    g_fake.ret[FAKE_GET_OMR_PREFIX] = 13;
    routing_publisher_init(&s_pub, &fake_ops);
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_ADD_ON_MESH));

    g_fake.ret[FAKE_GET_OMR_PREFIX] = 0;
    routing_publisher_sync_default_route(&s_pub);
    const fake_call_t *add = fake_ops_find(FAKE_ADD_ON_MESH, 0);
    TEST_ASSERT_NOT_NULL(add);
    TEST_ASSERT_TRUE(call_has_prefix(add, &ULA));
    TEST_ASSERT_EQUAL(ROUTING_PREF_LOW, add->pref);
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REGISTER_NETDATA, 0) > fake_ops_index(FAKE_ADD_ON_MESH, 0));
}

/* --- apply: custom OMR, ULA box clear --- */

TEST_CASE("Custom OMR, ULA box clear: sets the custom OMR config with the prefix and preference medium", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, false);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    const fake_call_t *c = fake_ops_find(FAKE_SET_OMR_CONFIG, 0);
    TEST_ASSERT_EQUAL(ROUTING_OMR_CUSTOM, c->mode);
    TEST_ASSERT_TRUE(call_has_prefix(c, &CUSTOM));
    TEST_ASSERT_EQUAL(ROUTING_PREF_MED, c->pref);
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_ADD_ON_MESH));
}

TEST_CASE("Custom OMR, ULA box clear: removes the ULA on-mesh entry if it added one", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, false);
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_REMOVE_ON_MESH)); /* none added: nothing to remove */

    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, false);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_REMOVE_ON_MESH));
    TEST_ASSERT_TRUE(call_has_prefix(fake_ops_find(FAKE_REMOVE_ON_MESH, 0), &ULA));
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REGISTER_NETDATA, 0) > fake_ops_index(FAKE_REMOVE_ON_MESH, 0));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_SET_OMR_CONFIG)); /* the custom config did not change */
}

/* --- apply: custom OMR, ULA box ticked --- */

TEST_CASE("Custom OMR, ULA box ticked: sets the custom OMR config with preference medium", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    const fake_call_t *c = fake_ops_find(FAKE_SET_OMR_CONFIG, 0);
    TEST_ASSERT_EQUAL(ROUTING_OMR_CUSTOM, c->mode);
    TEST_ASSERT_TRUE(call_has_prefix(c, &CUSTOM));
    TEST_ASSERT_EQUAL(ROUTING_PREF_MED, c->pref);
}

TEST_CASE("Custom OMR, ULA box ticked: adds the ULA as a SLAAC on-mesh prefix with preference low and registers "
          "Network Data", "[routing]")
{
    start();
    g_fake.default_route = true;
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_ADD_ON_MESH));
    const fake_call_t *add = fake_ops_find(FAKE_ADD_ON_MESH, 0);
    TEST_ASSERT_TRUE(call_has_prefix(add, &ULA));
    TEST_ASSERT_EQUAL(ROUTING_PREF_LOW, add->pref);
    TEST_ASSERT_TRUE(add->flag); /* follows default_route_published */
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_REGISTER_NETDATA));
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REGISTER_NETDATA, 0) > fake_ops_index(FAKE_ADD_ON_MESH, 0));
}

TEST_CASE("Custom OMR, ULA box ticked: the custom prefix stays the favored OMR prefix", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    const fake_call_t *set = fake_ops_find(FAKE_SET_OMR_CONFIG, 0);
    const fake_call_t *add = fake_ops_find(FAKE_ADD_ON_MESH, 0);
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_NOT_NULL(add);
    TEST_ASSERT_TRUE(add->pref < set->pref); /* ULA low below the custom medium */
    /* custom first, so OpenThread withdraws its own ULA OMR entry before ours goes in */
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_ADD_ON_MESH, 0) > fake_ops_index(FAKE_SET_OMR_CONFIG, 0));
}

/* --- apply: auto OMR --- */

TEST_CASE("Auto OMR: restores the automatic OMR config and removes the ULA on-mesh entry if it added one",
          "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_AUTO, NULL, true);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    const fake_call_t *set = fake_ops_find(FAKE_SET_OMR_CONFIG, 0);
    TEST_ASSERT_EQUAL(ROUTING_OMR_AUTO, set->mode);
    TEST_ASSERT_FALSE(set->has_prefix);
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_REMOVE_ON_MESH));
    TEST_ASSERT_TRUE(call_has_prefix(fake_ops_find(FAKE_REMOVE_ON_MESH, 0), &ULA));
    /* ours goes first: once auto is set OpenThread publishes the same ULA from the same local Network Data */
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REMOVE_ON_MESH, 0) < fake_ops_index(FAKE_SET_OMR_CONFIG, 0));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_ADD_ON_MESH));
}

/* --- apply: prefixes of others, sessions --- */

TEST_CASE("Prefix added through /add_prefix: is left untouched", "[routing]")
{
    start();
    fake_ops_netdata_add(&OTHER);
    apply(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    routing_publisher_sync_default_route(&s_pub);
    apply(ROUTING_METHOD_RIPNG, ROUTING_OMR_CUSTOM, &CUSTOM, false);
    apply(ROUTING_METHOD_OFF, ROUTING_OMR_AUTO, NULL, true);

    for (int i = 0; i < g_fake.call_count; i++) {
        const fake_call_t *c = &g_fake.calls[i];
        if (c->op == FAKE_ADD_ON_MESH || c->op == FAKE_REMOVE_ON_MESH) {
            TEST_ASSERT_TRUE(call_has_prefix(c, &ULA));
        }
    }
    TEST_ASSERT_TRUE(fake_ops_count(FAKE_ADD_ON_MESH) > 0); /* the loop checked something */
}

TEST_CASE("BGP session down or no RIPng neighbour: changes nothing on the mesh", "[routing]")
{
    start();
    fake_ops_netdata_add(&CUSTOM);
    fake_ops_netdata_add(&ULA);
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    fake_ops_clear_calls();

    /* what the routing task keeps calling whatever the sessions do */
    for (int i = 0; i < 5; i++) {
        routing_announce_input_t io = {0};
        routing_publisher_sync_default_route(&s_pub);
        routing_publisher_prefix_state(&s_pub, &io);
    }
    TEST_ASSERT_EQUAL(0, fake_ops_mutations());
}

/* --- default route flag --- */

TEST_CASE("OpenThread starts or stops publishing ::/0: the ULA on-mesh entry's default-route flag follows it",
          "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    TEST_ASSERT_FALSE(fake_ops_find(FAKE_ADD_ON_MESH, 0)->flag);
    fake_ops_clear_calls();

    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(0, fake_ops_mutations()); /* unchanged: nothing re-added */

    g_fake.default_route = true;
    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_ADD_ON_MESH));
    const fake_call_t *add = fake_ops_find(FAKE_ADD_ON_MESH, 0);
    TEST_ASSERT_TRUE(call_has_prefix(add, &ULA));
    TEST_ASSERT_EQUAL(ROUTING_PREF_LOW, add->pref);
    TEST_ASSERT_TRUE(add->flag);
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REGISTER_NETDATA, 0) > fake_ops_index(FAKE_ADD_ON_MESH, 0));

    g_fake.default_route = false;
    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(2, fake_ops_count(FAKE_ADD_ON_MESH));
    TEST_ASSERT_FALSE(fake_ops_find(FAKE_ADD_ON_MESH, 1)->flag);
    TEST_ASSERT_EQUAL(2, fake_ops_count(FAKE_REGISTER_NETDATA));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_REMOVE_ON_MESH));
}

TEST_CASE("OpenThread starts publishing ::/0 without a ULA entry of ours: adds nothing", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_AUTO, NULL, true);
    fake_ops_clear_calls();
    g_fake.default_route = true;
    routing_publisher_sync_default_route(&s_pub);

    TEST_ASSERT_EQUAL(0, g_fake.call_count);
}

/* --- apply_method --- */

TEST_CASE("Method pd: enables the DHCPv6-PD client, as today", "[routing]")
{
    start();
    apply(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_PD_SET_ENABLED));
    TEST_ASSERT_TRUE(fake_ops_find(FAKE_PD_SET_ENABLED, 0)->flag);
}

TEST_CASE("Method ripng, bgp or off: disables the DHCPv6-PD client", "[routing]")
{
    const routing_method_t methods[] = {ROUTING_METHOD_RIPNG, ROUTING_METHOD_BGP, ROUTING_METHOD_OFF};

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        start();
        apply(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);
        fake_ops_clear_calls();
        apply(methods[i], ROUTING_OMR_AUTO, NULL, false);
        TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_PD_SET_ENABLED));
        TEST_ASSERT_FALSE(fake_ops_find(FAKE_PD_SET_ENABLED, 0)->flag);
    }
}

/* --- apply: repeats, changes, failures --- */

TEST_CASE("Same settings applied twice: makes no OpenThread calls the second time", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    TEST_ASSERT_EQUAL(0, g_fake.call_count);

    apply(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);
    TEST_ASSERT_EQUAL(0, g_fake.call_count);
}

TEST_CASE("Custom prefix changed: sets the new custom OMR config and leaves the ULA entry", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM2, true);

    TEST_ASSERT_EQUAL(1, g_fake.call_count);
    const fake_call_t *set = fake_ops_find(FAKE_SET_OMR_CONFIG, 0);
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_EQUAL(ROUTING_OMR_CUSTOM, set->mode);
    TEST_ASSERT_TRUE(call_has_prefix(set, &CUSTOM2));
}

TEST_CASE("Custom prefix equal to the ULA, box ticked: adds no ULA entry", "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &ULA, true);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_ADD_ON_MESH));
}

TEST_CASE("An OMR config or PD call failed: the next apply issues it again", "[routing]")
{
    start();
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 7; /* OT_ERROR_INVALID_ARGS */
    g_fake.ret[FAKE_PD_SET_ENABLED] = 1;
    apply(ROUTING_METHOD_PD, ROUTING_OMR_CUSTOM, &CUSTOM, false);
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 0;
    g_fake.ret[FAKE_PD_SET_ENABLED] = 0;
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_PD, ROUTING_OMR_CUSTOM, &CUSTOM, false);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_PD_SET_ENABLED));
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_PD, ROUTING_OMR_CUSTOM, &CUSTOM, false);
    TEST_ASSERT_EQUAL(0, g_fake.call_count);
}

TEST_CASE("OMR config failed with the box ticked: adds no ULA entry", "[routing]")
{
    start();
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 7; /* OT_ERROR_INVALID_ARGS */
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    routing_publisher_sync_default_route(&s_pub);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_ADD_ON_MESH)); /* OpenThread's own ULA entry stays untouched */
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_REGISTER_NETDATA));
}

TEST_CASE("OMR config failed with the box ticked: a later successful apply adds the ULA entry", "[routing]")
{
    start();
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 7;
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 0;
    fake_ops_clear_calls();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_SET_OMR_CONFIG));
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_ADD_ON_MESH));
    TEST_ASSERT_TRUE(call_has_prefix(fake_ops_find(FAKE_ADD_ON_MESH, 0), &ULA));
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_ADD_ON_MESH, 0) > fake_ops_index(FAKE_SET_OMR_CONFIG, 0));
    TEST_ASSERT_TRUE(fake_ops_index(FAKE_REGISTER_NETDATA, 0) > fake_ops_index(FAKE_ADD_ON_MESH, 0));
}

TEST_CASE("Adding the ULA entry failed: sync_default_route adds it again without registering the failure",
          "[routing]")
{
    start();
    g_fake.ret[FAKE_ADD_ON_MESH] = 3; /* OT_ERROR_NO_BUFS */
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_ADD_ON_MESH));
    TEST_ASSERT_EQUAL(0, fake_ops_count(FAKE_REGISTER_NETDATA));

    g_fake.ret[FAKE_ADD_ON_MESH] = 0;
    fake_ops_clear_calls();
    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_ADD_ON_MESH));
    TEST_ASSERT_EQUAL(1, fake_ops_count(FAKE_REGISTER_NETDATA));
    fake_ops_clear_calls();
    routing_publisher_sync_default_route(&s_pub);
    TEST_ASSERT_EQUAL(0, fake_ops_mutations());
}

/* --- prefix_state --- */

TEST_CASE("After apply: reports ula, ula_known, and whether the custom prefix and the ULA are in Network Data",
          "[routing]")
{
    start();
    apply(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    routing_announce_input_t io;

    memset(&io, 0, sizeof(io));
    io.method = ROUTING_METHOD_BGP;
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_TRUE(io.ula_known);
    assert_prefix(&ULA, &io.ula);
    TEST_ASSERT_FALSE(io.custom_in_netdata);
    TEST_ASSERT_FALSE(io.ula_in_netdata);
    TEST_ASSERT_EQUAL(ROUTING_METHOD_BGP, io.method); /* other fields left alone */

    fake_ops_netdata_add(&CUSTOM);
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_TRUE(io.custom_in_netdata);
    TEST_ASSERT_FALSE(io.ula_in_netdata);

    fake_ops_netdata_add(&ULA);
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_TRUE(io.custom_in_netdata);
    TEST_ASSERT_TRUE(io.ula_in_netdata);

    apply(ROUTING_METHOD_BGP, ROUTING_OMR_AUTO, NULL, true);
    routing_publisher_prefix_state(&s_pub, &io);
    TEST_ASSERT_FALSE(io.custom_in_netdata); /* auto: no custom prefix, even if one lingers in Network Data */
    TEST_ASSERT_TRUE(io.ula_in_netdata);
}

/* --- apply: result --- */

TEST_CASE("Everything applied: apply returns true", "[routing]")
{
    start();
    routing_settings_t s = settings(ROUTING_METHOD_PD, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    TEST_ASSERT_TRUE(routing_publisher_apply(&s_pub, &s));
    TEST_ASSERT_TRUE(routing_publisher_apply(&s_pub, &s)); /* nothing left to issue */
}

TEST_CASE("OMR config call failed: apply returns false", "[routing]")
{
    start();
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 7;
    routing_settings_t s = settings(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, false);

    TEST_ASSERT_FALSE(routing_publisher_apply(&s_pub, &s));
}

TEST_CASE("PD call failed: apply returns false", "[routing]")
{
    start();
    g_fake.ret[FAKE_PD_SET_ENABLED] = 1;
    routing_settings_t s = settings(ROUTING_METHOD_PD, ROUTING_OMR_AUTO, NULL, false);

    TEST_ASSERT_FALSE(routing_publisher_apply(&s_pub, &s));
}

TEST_CASE("Adding the ULA entry failed: apply returns false", "[routing]")
{
    start();
    g_fake.ret[FAKE_ADD_ON_MESH] = 3;
    routing_settings_t s = settings(ROUTING_METHOD_BGP, ROUTING_OMR_CUSTOM, &CUSTOM, true);

    TEST_ASSERT_FALSE(routing_publisher_apply(&s_pub, &s));
}

TEST_CASE("A failed call succeeds on retry: apply returns true", "[routing]")
{
    start();
    g_fake.ret[FAKE_SET_OMR_CONFIG] = 7;
    g_fake.ret[FAKE_PD_SET_ENABLED] = 1;
    routing_settings_t s = settings(ROUTING_METHOD_PD, ROUTING_OMR_CUSTOM, &CUSTOM, true);
    TEST_ASSERT_FALSE(routing_publisher_apply(&s_pub, &s));

    g_fake.ret[FAKE_SET_OMR_CONFIG] = 0;
    TEST_ASSERT_FALSE(routing_publisher_apply(&s_pub, &s)); /* PD still failing */
    g_fake.ret[FAKE_PD_SET_ENABLED] = 0;
    TEST_ASSERT_TRUE(routing_publisher_apply(&s_pub, &s));
}
