// Prefix publisher (spec section 5). Pure: every OpenThread call goes through routing_ot_ops_t.

#include "publisher.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "routing";
static const routing_prefix64_t NO_PREFIX;

static bool prefix_eq(const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

static void learn_ula(routing_publisher_t *p)
{
    routing_prefix64_t ula;

    if (p->ops->get_omr_prefix(&ula) == 0) {
        p->ula = ula;
        p->ula_known = true;
    }
}

// The extra ULA entry belongs on the mesh: custom mode, the box ticked, the ULA known and not the custom prefix itself
// (OpenThread already publishes that one as its local OMR prefix).
static bool ula_wanted(const routing_publisher_t *p)
{
    return p->have_settings && p->want_omr_mode == ROUTING_OMR_CUSTOM && p->want_announce_ula && p->ula_known &&
           !prefix_eq(&p->ula, &p->want_custom);
}

static void note_ula_result(routing_publisher_t *p, int err, const char *what)
{
    if (err == 0) {
        p->ula_failed = false;
        return;
    }
    if (!p->ula_failed) {
        ESP_LOGW(TAG, "%s the ULA on-mesh prefix failed: %d", what, err);
    }
    p->ula_failed = true;
}

static void add_ula(routing_publisher_t *p, bool default_route)
{
    int err = p->ops->add_on_mesh(&p->ula, ROUTING_PREF_LOW, default_route);

    note_ula_result(p, err, "adding");
    if (err != 0) {
        return;
    }
    p->ops->register_netdata();
    p->ula_published = true;
    p->ula_default_route = default_route;
}

// The entry is gone afterwards either way: OpenThread's only removal error is "not found".
static void remove_ula(routing_publisher_t *p)
{
    note_ula_result(p, p->ops->remove_on_mesh(&p->ula), "removing");
    p->ops->register_netdata();
    p->ula_published = false;
}

static void remove_ula_if_unwanted(routing_publisher_t *p)
{
    if (p->ula_published && !ula_wanted(p)) {
        remove_ula(p);
    }
}

// Add the entry when it is wanted and missing; with `follow_flag`, also re-add it when the ::/0 flag changed.
static void add_ula_if_wanted(routing_publisher_t *p, bool follow_flag)
{
    if (!ula_wanted(p)) {
        return;
    }
    if (!p->ula_published) {
        add_ula(p, p->ops->default_route_published());
        return;
    }
    if (follow_flag) {
        bool default_route = p->ops->default_route_published();
        if (default_route != p->ula_default_route) {
            add_ula(p, default_route);
        }
    }
}

static void apply_omr(routing_publisher_t *p)
{
    bool custom = p->want_omr_mode == ROUTING_OMR_CUSTOM;

    if (p->omr_applied && p->omr_mode == p->want_omr_mode &&
            (!custom || prefix_eq(&p->omr_prefix, &p->want_custom))) {
        return;
    }
    int err = p->ops->set_omr_config(p->want_omr_mode, custom ? &p->want_custom : NULL, ROUTING_PREF_MED);
    if (err != 0) {
        ESP_LOGW(TAG, "setting the %s OMR config failed: %d", custom ? "custom" : "auto", err);
        p->omr_applied = false;
        return;
    }
    p->omr_applied = true;
    p->omr_mode = p->want_omr_mode;
    p->omr_prefix = custom ? p->want_custom : NO_PREFIX;
}

static void apply_pd(routing_publisher_t *p, bool enabled)
{
    if (p->pd_applied && p->pd_enabled == enabled) {
        return;
    }
    int err = p->ops->pd_set_enabled(enabled);
    if (err != 0) {
        ESP_LOGW(TAG, "switching DHCPv6-PD %s failed: %d", enabled ? "on" : "off", err);
        p->pd_applied = false;
        return;
    }
    p->pd_applied = true;
    p->pd_enabled = enabled;
}

void routing_publisher_init(routing_publisher_t *p, const routing_ot_ops_t *ops)
{
    memset(p, 0, sizeof(*p));
    p->ops = ops;
    learn_ula(p);
}

void routing_publisher_apply(routing_publisher_t *p, const routing_settings_t *s)
{
    p->have_settings = true;
    p->want_omr_mode = s->omr_mode;
    p->want_custom = s->omr_mode == ROUTING_OMR_CUSTOM ? s->omr_prefix : NO_PREFIX;
    p->want_announce_ula = s->announce_ula;

    // Order matters because the ULA entry and OpenThread's own auto OMR prefix are the same prefix in the local
    // Network Data: our entry goes before the auto config makes OpenThread publish the ULA (or removing ours would
    // remove its entry), and comes after the custom config made OpenThread withdraw its own.
    remove_ula_if_unwanted(p);
    apply_omr(p);
    add_ula_if_wanted(p, false);
    apply_pd(p, s->method == ROUTING_METHOD_PD);
}

void routing_publisher_sync_default_route(routing_publisher_t *p)
{
    if (!p->ula_known) {
        learn_ula(p);
    }
    add_ula_if_wanted(p, true);
}

void routing_publisher_prefix_state(const routing_publisher_t *p, routing_announce_input_t *io)
{
    io->ula_known = p->ula_known;
    io->ula = p->ula_known ? p->ula : NO_PREFIX;
    io->custom_in_netdata = p->have_settings && p->want_omr_mode == ROUTING_OMR_CUSTOM &&
                            p->ops->netdata_has_prefix(&p->want_custom);
    io->ula_in_netdata = p->ula_known && p->ops->netdata_has_prefix(&p->ula);
}
