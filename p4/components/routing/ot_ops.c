// routing_ot_ops_t on OpenThread (target only). The caller holds the OpenThread lock (publisher.h).

#include "publisher.h"

#include <string.h>

#include "esp_openthread.h"
#include "openthread/border_router.h"
#include "openthread/border_routing.h"
#include "openthread/netdata.h"
#include "openthread/thread.h"

static otRoutePreference to_ot_pref(int8_t pref)
{
    return pref < 0 ? OT_ROUTE_PREFERENCE_LOW : OT_ROUTE_PREFERENCE_MED;
}

static void to_ot_prefix(const routing_prefix64_t *in, otIp6Prefix *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->mPrefix.mFields.m8, in->b, sizeof(in->b));
    out->mLength = 64;
}

static bool is_prefix64(const otIp6Prefix *ot, const routing_prefix64_t *p)
{
    return ot->mLength == 64 && memcmp(ot->mPrefix.mFields.m8, p->b, sizeof(p->b)) == 0;
}

static int ot_set_omr_config(routing_omr_mode_t mode, const routing_prefix64_t *prefix, int8_t pref)
{
    otInstance *instance = esp_openthread_get_instance();

    if (mode == ROUTING_OMR_AUTO) {
        return otBorderRoutingSetOmrConfig(instance, OT_BORDER_ROUTING_OMR_CONFIG_AUTO, NULL, to_ot_pref(pref));
    }
    otIp6Prefix ot_prefix;
    to_ot_prefix(prefix, &ot_prefix);
    return otBorderRoutingSetOmrConfig(instance, OT_BORDER_ROUTING_OMR_CONFIG_CUSTOM, &ot_prefix, to_ot_pref(pref));
}

static int ot_get_omr_prefix(routing_prefix64_t *out)
{
    otIp6Prefix ot_prefix;
    otError err = otBorderRoutingGetOmrPrefix(esp_openthread_get_instance(), &ot_prefix);

    if (err == OT_ERROR_NONE) {
        if (ot_prefix.mLength != 64) {
            return OT_ERROR_INVALID_STATE;
        }
        memcpy(out->b, ot_prefix.mPrefix.mFields.m8, sizeof(out->b));
    }
    return err;
}

static int ot_add_on_mesh(const routing_prefix64_t *prefix, int8_t pref, bool default_route)
{
    otBorderRouterConfig config;

    memset(&config, 0, sizeof(config));
    to_ot_prefix(prefix, &config.mPrefix);
    config.mPreference = to_ot_pref(pref);
    config.mPreferred = true;
    config.mSlaac = true;
    config.mOnMesh = true;
    config.mStable = true;
    config.mDefaultRoute = default_route;
    return otBorderRouterAddOnMeshPrefix(esp_openthread_get_instance(), &config);
}

static int ot_remove_on_mesh(const routing_prefix64_t *prefix)
{
    otIp6Prefix ot_prefix;

    to_ot_prefix(prefix, &ot_prefix);
    return otBorderRouterRemoveOnMeshPrefix(esp_openthread_get_instance(), &ot_prefix);
}

static int ot_register_netdata(void)
{
    return otBorderRouterRegister(esp_openthread_get_instance());
}

static int ot_pd_set_enabled(bool enabled)
{
    otBorderRoutingDhcp6PdSetEnabled(esp_openthread_get_instance(), enabled);
    return OT_ERROR_NONE;
}

// The leader Network Data has the /64 as an on-mesh prefix (from any border router).
static bool ot_netdata_has_prefix(const routing_prefix64_t *prefix)
{
    otInstance *instance = esp_openthread_get_instance();
    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig config;

    while (otNetDataGetNextOnMeshPrefix(instance, &it, &config) == OT_ERROR_NONE) {
        if (is_prefix64(&config.mPrefix, prefix)) {
            return true;
        }
    }
    return false;
}

// The leader Network Data has ::/0 as an external route from this device's RLOC16.
static bool ot_default_route_published(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otExternalRouteConfig config;
    uint16_t rloc16 = otThreadGetRloc16(instance);

    while (otNetDataGetNextRoute(instance, &it, &config) == OT_ERROR_NONE) {
        if (config.mPrefix.mLength == 0 && config.mRloc16 == rloc16) {
            return true;
        }
    }
    return false;
}

const routing_ot_ops_t routing_ot_ops_openthread = {
    .set_omr_config = ot_set_omr_config,
    .get_omr_prefix = ot_get_omr_prefix,
    .add_on_mesh = ot_add_on_mesh,
    .remove_on_mesh = ot_remove_on_mesh,
    .register_netdata = ot_register_netdata,
    .pd_set_enabled = ot_pd_set_enabled,
    .netdata_has_prefix = ot_netdata_has_prefix,
    .default_route_published = ot_default_route_published,
};
