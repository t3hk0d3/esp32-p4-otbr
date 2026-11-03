#include "fake_ops.h"

#include <string.h>

#include "unity.h"

fake_ops_state_t g_fake;

static fake_call_t *record(fake_op_t op, const routing_prefix64_t *prefix)
{
    if (g_fake.call_count >= FAKE_MAX_CALLS) {
        TEST_FAIL_MESSAGE("fake_ops: more than FAKE_MAX_CALLS calls recorded");
    }
    fake_call_t *c = &g_fake.calls[g_fake.call_count++];
    memset(c, 0, sizeof(*c));
    c->op = op;
    if (prefix) {
        c->has_prefix = true;
        c->prefix = *prefix;
    }
    return c;
}

static int fake_set_omr_config(routing_omr_mode_t mode, const routing_prefix64_t *prefix, int8_t pref)
{
    fake_call_t *c = record(FAKE_SET_OMR_CONFIG, prefix);
    if (c) {
        c->mode = mode;
        c->pref = pref;
    }
    return g_fake.ret[FAKE_SET_OMR_CONFIG];
}

static int fake_get_omr_prefix(routing_prefix64_t *out)
{
    record(FAKE_GET_OMR_PREFIX, NULL);
    if (g_fake.ret[FAKE_GET_OMR_PREFIX] == 0) {
        *out = g_fake.omr_prefix;
    }
    return g_fake.ret[FAKE_GET_OMR_PREFIX];
}

static int fake_add_on_mesh(const routing_prefix64_t *prefix, int8_t pref, bool default_route)
{
    fake_call_t *c = record(FAKE_ADD_ON_MESH, prefix);
    if (c) {
        c->pref = pref;
        c->flag = default_route;
    }
    return g_fake.ret[FAKE_ADD_ON_MESH];
}

static int fake_remove_on_mesh(const routing_prefix64_t *prefix)
{
    record(FAKE_REMOVE_ON_MESH, prefix);
    return g_fake.ret[FAKE_REMOVE_ON_MESH];
}

static int fake_register_netdata(void)
{
    record(FAKE_REGISTER_NETDATA, NULL);
    return g_fake.ret[FAKE_REGISTER_NETDATA];
}

static int fake_pd_set_enabled(bool enabled)
{
    fake_call_t *c = record(FAKE_PD_SET_ENABLED, NULL);
    if (c) {
        c->flag = enabled;
    }
    return g_fake.ret[FAKE_PD_SET_ENABLED];
}

static bool fake_netdata_has_prefix(const routing_prefix64_t *prefix)
{
    record(FAKE_NETDATA_HAS_PREFIX, prefix);
    for (int i = 0; i < g_fake.netdata_count; i++) {
        if (memcmp(g_fake.netdata[i].b, prefix->b, sizeof(prefix->b)) == 0) {
            return true;
        }
    }
    return false;
}

static bool fake_default_route_published(void)
{
    record(FAKE_DEFAULT_ROUTE_PUBLISHED, NULL);
    return g_fake.default_route;
}

const routing_ot_ops_t fake_ops = {
    .set_omr_config = fake_set_omr_config,
    .get_omr_prefix = fake_get_omr_prefix,
    .add_on_mesh = fake_add_on_mesh,
    .remove_on_mesh = fake_remove_on_mesh,
    .register_netdata = fake_register_netdata,
    .pd_set_enabled = fake_pd_set_enabled,
    .netdata_has_prefix = fake_netdata_has_prefix,
    .default_route_published = fake_default_route_published,
};

void fake_ops_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
}

void fake_ops_clear_calls(void)
{
    memset(g_fake.calls, 0, sizeof(g_fake.calls));
    g_fake.call_count = 0;
}

void fake_ops_netdata_add(const routing_prefix64_t *prefix)
{
    if (g_fake.netdata_count < FAKE_MAX_NETDATA) {
        g_fake.netdata[g_fake.netdata_count++] = *prefix;
    }
}

int fake_ops_index(fake_op_t op, int nth)
{
    for (int i = 0; i < g_fake.call_count; i++) {
        if (g_fake.calls[i].op == op && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

int fake_ops_count(fake_op_t op)
{
    int n = 0;
    for (int i = 0; i < g_fake.call_count; i++) {
        n += g_fake.calls[i].op == op;
    }
    return n;
}

const fake_call_t *fake_ops_find(fake_op_t op, int nth)
{
    int i = fake_ops_index(op, nth);
    return i < 0 ? NULL : &g_fake.calls[i];
}

int fake_ops_mutations(void)
{
    return g_fake.call_count - fake_ops_count(FAKE_GET_OMR_PREFIX) - fake_ops_count(FAKE_NETDATA_HAS_PREFIX) -
           fake_ops_count(FAKE_DEFAULT_ROUTE_PUBLISHED);
}
