#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_config.h"

static esp_ip4_addr_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    esp_ip4_addr_t addr;
    uint8_t *bytes = (uint8_t *)&addr.addr;

    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
    return addr;
}

static netcfg_iface_cfg_t static_iface(uint16_t vlan, esp_ip4_addr_t addr, esp_ip4_addr_t mask)
{
    return (netcfg_iface_cfg_t) {
        .vlan = vlan, .dhcp = false, .addr = addr, .mask = mask
    };
}

static netcfg_config_t split_config(uint16_t mgmt_vlan, uint16_t bb_vlan)
{
    netcfg_config_t cfg;

    netcfg_defaults(&cfg);
    cfg.mode = NETCFG_MODE_SPLIT;
    cfg.mgmt.vlan = mgmt_vlan;
    cfg.thread_bb.vlan = bb_vlan;
    return cfg;
}

TEST_CASE("On the defaults: single mode, untagged, DHCPv4, valid", "[netcfg]")
{
    netcfg_config_t cfg;
    const char *bad = NULL;

    netcfg_defaults(&cfg);
    TEST_ASSERT_EQUAL(NETCFG_MODE_SINGLE, cfg.mode);
    TEST_ASSERT_EQUAL(0, cfg.single.vlan);
    TEST_ASSERT_TRUE(cfg.single.dhcp);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_validate(&cfg, &bad));
}

TEST_CASE("On split mode with two untagged interfaces: rejects with field vlan", "[netcfg]")
{
    netcfg_config_t cfg = split_config(0, 0);
    const char *bad = NULL;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("vlan", bad);
}

TEST_CASE("On split mode with equal VLAN IDs: rejects with field vlan", "[netcfg]")
{
    netcfg_config_t cfg = split_config(20, 20);
    const char *bad = NULL;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("vlan", bad);
}

TEST_CASE("On VLAN ID 0 (untagged) or 1..4094: accepts", "[netcfg]")
{
    const uint16_t ids[] = {0, 1, 2, 100, 4093, 4094};

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        netcfg_config_t single;
        netcfg_defaults(&single);
        single.single.vlan = ids[i];
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, netcfg_validate(&single, NULL), "single");
        netcfg_config_t split = split_config(ids[i], ids[i] == 7 ? 8 : 7);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, netcfg_validate(&split, NULL), "split");
    }
}

TEST_CASE("On VLAN ID 4095 or above: rejects with field vlan", "[netcfg]")
{
    const uint16_t ids[] = {4095, 4096, 65535};

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        netcfg_config_t cfg;
        const char *bad = NULL;

        netcfg_defaults(&cfg);
        cfg.single.vlan = ids[i];
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
        TEST_ASSERT_EQUAL_STRING("vlan", bad);
    }
}

TEST_CASE("On static IPv4 with non-contiguous mask: rejects with field ipv4.mask", "[netcfg]")
{
    netcfg_config_t cfg;
    const char *bad = NULL;

    netcfg_defaults(&cfg);
    cfg.single = static_iface(0, ip(192, 168, 1, 5), ip(255, 0, 255, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("ipv4.mask", bad);
    cfg.single.mask = ip(0, 0, 0, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("ipv4.mask", bad);
    cfg.single.mask = ip(255, 255, 255, 0);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_validate(&cfg, &bad));
}

TEST_CASE("On static IPv4 with an unusable address: rejects with field ipv4.addr", "[netcfg]")
{
    const esp_ip4_addr_t bad_addrs[] = {ip(0, 0, 0, 0), ip(127, 0, 0, 1), ip(224, 0, 0, 5), ip(255, 255, 255, 255),
                                        ip(192, 168, 1, 0), ip(192, 168, 1, 255)
                                       };

    for (size_t i = 0; i < sizeof(bad_addrs) / sizeof(bad_addrs[0]); i++) {
        netcfg_config_t cfg;
        const char *bad = NULL;

        netcfg_defaults(&cfg);
        cfg.single = static_iface(0, bad_addrs[i], ip(255, 255, 255, 0));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
        TEST_ASSERT_EQUAL_STRING("ipv4.addr", bad);
    }
}

TEST_CASE("On static IPv4 with a gateway outside the subnet or a multicast DNS: rejects the field", "[netcfg]")
{
    netcfg_config_t cfg;
    const char *bad = NULL;

    netcfg_defaults(&cfg);
    cfg.single = static_iface(0, ip(192, 168, 1, 5), ip(255, 255, 255, 0));
    cfg.single.gw = ip(192, 168, 2, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("ipv4.gw", bad);
    cfg.single.gw = ip(192, 168, 1, 1);
    cfg.single.dns = ip(224, 0, 0, 251);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("ipv4.dns", bad);
    cfg.single.dns = ip(192, 168, 1, 2);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_validate(&cfg, &bad));
}

TEST_CASE("On split mode with static IPv4 interfaces in one subnet: rejects with field ipv4.addr", "[netcfg]")
{
    netcfg_config_t cfg = split_config(10, 20);
    const char *bad = NULL;

    cfg.mgmt = static_iface(10, ip(192, 168, 1, 5), ip(255, 255, 255, 0));
    cfg.thread_bb = static_iface(20, ip(192, 168, 1, 6), ip(255, 255, 255, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("ipv4.addr", bad);
    cfg.thread_bb.addr = ip(192, 168, 2, 6);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_validate(&cfg, &bad));
}

TEST_CASE("On an unknown mode: rejects with field mode", "[netcfg]")
{
    netcfg_config_t cfg;
    const char *bad = NULL;

    netcfg_defaults(&cfg);
    cfg.mode = (netcfg_mode_t)7;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_validate(&cfg, &bad));
    TEST_ASSERT_EQUAL_STRING("mode", bad);
}

static void assert_round_trip(const netcfg_config_t *cfg)
{
    char *json = netcfg_to_json(cfg);
    netcfg_config_t back;
    const char *bad = NULL;

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, netcfg_from_json(json, &back, &bad), json);
    TEST_ASSERT_TRUE_MESSAGE(netcfg_equal(cfg, &back), json);
    free(json);
}

TEST_CASE("On JSON round trip: netcfg_from_json(netcfg_to_json(cfg)) equals cfg", "[netcfg]")
{
    netcfg_config_t cfg;

    netcfg_defaults(&cfg);
    assert_round_trip(&cfg); // single, untagged, DHCP
    cfg.single = static_iface(30, ip(10, 1, 2, 3), ip(255, 255, 0, 0));
    cfg.single.gw = ip(10, 1, 0, 1);
    cfg.single.dns = ip(10, 1, 0, 2);
    assert_round_trip(&cfg); // single, tagged, static
    cfg = split_config(0, 20);
    cfg.thread_bb = static_iface(20, ip(172, 16, 0, 9), ip(255, 255, 255, 128));
    assert_round_trip(&cfg); // split, untagged mgmt with DHCP, tagged static backbone
    cfg.trel = false;
    assert_round_trip(&cfg); // TREL off
}

TEST_CASE("On JSON of the spec shape: parses into the config", "[netcfg]")
{
    static const char json[] =
        "{\"mode\":\"split\",\"mgmt\":{\"vlan\":10,\"ipv4\":{\"dhcp\":true}},"
        "\"thread_bb\":{\"vlan\":20,\"ipv4\":{\"dhcp\":false,\"addr\":\"192.168.20.2\",\"mask\":\"255.255.255.0\","
        "\"gw\":\"192.168.20.1\",\"dns\":\"192.168.20.1\"}}}";
    netcfg_config_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(json, &cfg, NULL));
    TEST_ASSERT_EQUAL(NETCFG_MODE_SPLIT, cfg.mode);
    TEST_ASSERT_EQUAL(10, cfg.mgmt.vlan);
    TEST_ASSERT_TRUE(cfg.mgmt.dhcp);
    TEST_ASSERT_EQUAL(20, cfg.thread_bb.vlan);
    TEST_ASSERT_FALSE(cfg.thread_bb.dhcp);
    TEST_ASSERT_TRUE(ip(192, 168, 20, 2).addr == cfg.thread_bb.addr.addr);
    TEST_ASSERT_TRUE(ip(255, 255, 255, 0).addr == cfg.thread_bb.mask.addr);
    TEST_ASSERT_TRUE(ip(192, 168, 20, 1).addr == cfg.thread_bb.gw.addr);
}

static void assert_rejected(const char *json, const char *field)
{
    netcfg_config_t cfg;
    const char *bad = NULL;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, netcfg_from_json(json, &cfg, &bad), json);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field, bad, json);
}

TEST_CASE("On malformed or incomplete JSON: rejects naming the field", "[netcfg]")
{
    assert_rejected("not json", "json");
    assert_rejected("[]", "json");
    assert_rejected("{}", "mode");
    assert_rejected("{\"mode\":\"bridge\"}", "mode");
    assert_rejected("{\"mode\":\"single\"}", "single");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0}}", "ipv4");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"ipv4\":{\"dhcp\":true}}}", "vlan"); // missing, not untagged
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":4095,\"ipv4\":{\"dhcp\":true}}}", "vlan");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":1.5,\"ipv4\":{\"dhcp\":true}}}", "vlan");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":\"7\",\"ipv4\":{\"dhcp\":true}}}", "vlan");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":false,\"mask\":\"255.255.255.0\"}}}", "ipv4.addr");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":false,\"addr\":\"10.0.0.300\",\"mask\":\"255.255.255.0\"}}}",
                    "ipv4.addr");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":false,\"addr\":\"10.0.0.2\",\"mask\":\"255.0.255.0\"}}}",
                    "ipv4.mask");
    assert_rejected("{\"mode\":\"split\",\"mgmt\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}}}", "thread_bb");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}}} trailing", "json");
}

TEST_CASE("On JSON longer than the limit: rejects with field json", "[netcfg]")
{
    char *big = malloc(NETCFG_JSON_MAX + 64);

    TEST_ASSERT_NOT_NULL(big);
    memset(big, ' ', NETCFG_JSON_MAX + 32);
    strcpy(big + NETCFG_JSON_MAX + 32, "{}");
    assert_rejected(big, "json");
    free(big);
}

TEST_CASE("On the defaults: TREL is on", "[netcfg]")
{
    netcfg_config_t cfg;

    netcfg_defaults(&cfg);
    TEST_ASSERT_TRUE(cfg.trel);
}

TEST_CASE("On JSON without trel (a config stored before TREL): parses with trel on", "[netcfg]")
{
    netcfg_config_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}}}", &cfg, NULL));
    TEST_ASSERT_TRUE(cfg.trel);
}

TEST_CASE("On JSON with trel false: parses with trel off", "[netcfg]")
{
    netcfg_config_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}},\"trel\":false}",
                                               &cfg, NULL));
    TEST_ASSERT_FALSE(cfg.trel);
}

TEST_CASE("On JSON with a trel that is not a boolean: rejects naming trel", "[netcfg]")
{
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}},\"trel\":1}", "trel");
    assert_rejected("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}},\"trel\":\"on\"}", "trel");
}

TEST_CASE("On a change request without trel: keeps the base value", "[netcfg]")
{
    netcfg_config_t base, cfg;

    netcfg_defaults(&base);
    base.trel = false;
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json_over("{\"mode\":\"single\",\"single\":{\"vlan\":3,\"ipv4\":{\"dhcp\":true}}}", &base,
                                                    &cfg, NULL));
    TEST_ASSERT_FALSE(cfg.trel);
    TEST_ASSERT_EQUAL(3, cfg.single.vlan);
}

TEST_CASE("On a change request with trel: the request wins over the base", "[netcfg]")
{
    netcfg_config_t base, cfg;

    netcfg_defaults(&base);
    base.trel = false;
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json_over("{\"mode\":\"single\",\"single\":{\"vlan\":0,\"ipv4\":{\"dhcp\":true}},\"trel\":true}",
                                                    &base, &cfg, NULL));
    TEST_ASSERT_TRUE(cfg.trel);
}

TEST_CASE("On configs differing only in trel: not equal, diff_only_trel true", "[netcfg]")
{
    netcfg_config_t a = split_config(10, 20), b = split_config(10, 20);

    b.trel = false;
    TEST_ASSERT_FALSE(netcfg_equal(&a, &b));
    TEST_ASSERT_TRUE(netcfg_diff_only_trel(&a, &b));
}

TEST_CASE("On equal configs: diff_only_trel false", "[netcfg]")
{
    netcfg_config_t a = split_config(10, 20), b = split_config(10, 20);

    TEST_ASSERT_FALSE(netcfg_diff_only_trel(&a, &b));
}

TEST_CASE("On configs differing in an interface or the mode: diff_only_trel false", "[netcfg]")
{
    netcfg_config_t a = split_config(10, 20), b = split_config(10, 21), c;

    b.trel = false;
    TEST_ASSERT_FALSE(netcfg_diff_only_trel(&a, &b));
    netcfg_defaults(&c);
    c.trel = false;
    TEST_ASSERT_FALSE(netcfg_diff_only_trel(&a, &c));
}
