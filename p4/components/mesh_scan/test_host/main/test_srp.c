#include <string.h>

#include "scan_types.h"
#include "srp.h"
#include "unity.h"

#define ENT_THREAD 44970
#define SVC_UNICAST 0x5d
#define SVC_ANYCAST 0x5c

// fd8f:11c6:352:a6a9:e1c7:e431:8422:bff7 as seen on the network
static const uint8_t SERVER_ADDR[16] = {0xfd, 0x8f, 0x11, 0xc6, 0x03, 0x52, 0xa6, 0xa9,
                                        0xe1, 0xc7, 0xe4, 0x31, 0x84, 0x22, 0xbf, 0xf7
                                       };
static const uint8_t PORT_BE[2] = {0xd1, 0x32}; // 53554

static const uint8_t OMR[8] = {0xfd, 0x87, 0x9b, 0x49, 0x82, 0x64, 0x00, 0x01};
static const uint8_t ML[8] = {0xfd, 0x8f, 0x11, 0xc6, 0x03, 0x52, 0xa6, 0xa9};
static const uint8_t XP[8] = {0x59, 0x65, 0xb5, 0x7e, 0x30, 0xc0, 0xde, 0xf4};
static const uint8_t XP_OTHER[8] = {0x59, 0x65, 0xb5, 0x7e, 0x30, 0xc0, 0xde, 0xf5};
static const uint8_t XA[8] = {0x7e, 0x28, 0x7c, 0x8b, 0x8b, 0x14, 0x0e, 0xe6};
static const uint8_t OWN_EXT[8] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

/** Append one TXT entry (length byte + text) and return the new length. */
static uint16_t txt_add(uint8_t *buf, uint16_t len, const void *text, uint8_t n)
{
    buf[len] = n;
    memcpy(buf + len + 1, text, n);
    return len + 1 + n;
}

static uint16_t txt_add_str(uint8_t *buf, uint16_t len, const char *s)
{
    return txt_add(buf, len, s, (uint8_t)strlen(s));
}

/** The meshcop TXT recorded from Home Assistant OTBR; xp and xa are replaceable. */
static uint16_t meshcop_txt(uint8_t *buf, const uint8_t *xp, const uint8_t *xa)
{
    uint8_t tmp[32];
    uint16_t n = 0;

    memcpy(tmp, "rv=1", 4);
    n = txt_add(buf, n, tmp, 4);
    n = txt_add_str(buf, n, "tv=1.4.0");
    if (xa) {
        memcpy(tmp, "xa=", 3);
        memcpy(tmp + 3, xa, 8);
        n = txt_add(buf, n, tmp, 11);
    }
    n = txt_add_str(buf, n, "nn=ha-thread-bbc1");
    if (xp) {
        memcpy(tmp, "xp=", 3);
        memcpy(tmp + 3, xp, 8);
        n = txt_add(buf, n, tmp, 11);
    }
    n = txt_add_str(buf, n, "at");
    n = txt_add_str(buf, n, "dn=DefaultDomain");
    return n;
}

static void ip6_with_prefix(uint8_t *addr, const uint8_t *prefix, uint8_t last)
{
    memcpy(addr, prefix, 8);
    memset(addr + 8, 0, 8);
    addr[15] = last;
}

// ---- mesh_scan_srp_server_parse ----

TEST_CASE("Enterprise 44970, service data 0x5d + 16-byte address + port: true, address and port from service data, "
          "rloc16 copied",
          "[mesh_scan]")
{
    uint8_t svc[19] = {SVC_UNICAST};
    mesh_scan_srp_server_t out;

    memcpy(svc + 1, SERVER_ADDR, 16);
    memcpy(svc + 17, PORT_BE, 2);
    memset(&out, 0xaa, sizeof(out));

    TEST_ASSERT_TRUE(mesh_scan_srp_server_parse(ENT_THREAD, svc, sizeof(svc), NULL, 0, 0x4c00, &out));
    TEST_ASSERT_EQUAL_MEMORY(SERVER_ADDR, out.addr, 16);
    TEST_ASSERT_EQUAL_UINT16(53554, out.port);
    TEST_ASSERT_EQUAL_UINT16(0x4c00, out.rloc16);
}

TEST_CASE("Service data 0x5d only, server data 16-byte address + port: true, address and port from server data",
          "[mesh_scan]")
{
    const uint8_t svc[1] = {SVC_UNICAST};
    uint8_t srv[18];
    mesh_scan_srp_server_t out;

    memcpy(srv, SERVER_ADDR, 16);
    memcpy(srv + 16, PORT_BE, 2);

    TEST_ASSERT_TRUE(mesh_scan_srp_server_parse(ENT_THREAD, svc, sizeof(svc), srv, sizeof(srv), 0x0800, &out));
    TEST_ASSERT_EQUAL_MEMORY(SERVER_ADDR, out.addr, 16);
    TEST_ASSERT_EQUAL_UINT16(53554, out.port);
    TEST_ASSERT_EQUAL_UINT16(0x0800, out.rloc16);
}

TEST_CASE("Service data 0x5c (anycast): false", "[mesh_scan]")
{
    uint8_t svc[19] = {SVC_ANYCAST};
    mesh_scan_srp_server_t out;

    memcpy(svc + 1, SERVER_ADDR, 16);
    memcpy(svc + 17, PORT_BE, 2);

    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD, svc, sizeof(svc), NULL, 0, 0x4c00, &out));
}

TEST_CASE("Other enterprise number: false", "[mesh_scan]")
{
    uint8_t svc[19] = {SVC_UNICAST};
    mesh_scan_srp_server_t out;

    memcpy(svc + 1, SERVER_ADDR, 16);
    memcpy(svc + 17, PORT_BE, 2);

    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD + 1, svc, sizeof(svc), NULL, 0, 0x4c00, &out));
}

TEST_CASE("Truncated service or server data: false", "[mesh_scan]")
{
    uint8_t svc[19] = {SVC_UNICAST};
    const uint8_t svc_only[1] = {SVC_UNICAST};
    uint8_t srv[18];
    mesh_scan_srp_server_t out;

    memcpy(svc + 1, SERVER_ADDR, 16);
    memcpy(srv, SERVER_ADDR, 16);

    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD, svc, 18, NULL, 0, 0x4c00, &out));
    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD, svc_only, 1, srv, 17, 0x4c00, &out));
    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD, svc_only, 1, NULL, 0, 0x4c00, &out));
    TEST_ASSERT_FALSE(mesh_scan_srp_server_parse(ENT_THREAD, svc, 0, NULL, 0, 0x4c00, &out));
}

// ---- mesh_scan_srp_addr_kept ----

TEST_CASE("An address under an OMR prefix: mesh_scan_srp_addr_kept returns true", "[mesh_scan]")
{
    mesh_scan_ip6_t addrs[2];
    mesh_scan_prefix_t omr[1];

    memcpy(omr[0], OMR, 8);
    ip6_with_prefix(addrs[0], (const uint8_t[8]) {
        0xfd, 0xaa, 0, 0x20, 0, 0, 0, 0
    }, 0x69);
    ip6_with_prefix(addrs[1], OMR, 0x42);

    TEST_ASSERT_TRUE(mesh_scan_srp_addr_kept(addrs, 2, omr, 1, ML));
}

TEST_CASE("Only a mesh-local EID: mesh_scan_srp_addr_kept returns true", "[mesh_scan]")
{
    mesh_scan_ip6_t addrs[1];
    mesh_scan_prefix_t omr[1];

    memcpy(omr[0], OMR, 8);
    ip6_with_prefix(addrs[0], ML, 0x99);

    TEST_ASSERT_TRUE(mesh_scan_srp_addr_kept(addrs, 1, omr, 1, ML));
}

TEST_CASE("Only LAN addresses: mesh_scan_srp_addr_kept returns false", "[mesh_scan]")
{
    mesh_scan_ip6_t addrs[2];
    mesh_scan_prefix_t omr[1];

    memcpy(omr[0], OMR, 8);
    ip6_with_prefix(addrs[0], (const uint8_t[8]) {
        0xfd, 0xaa, 0, 0x20, 0, 0, 0, 0
    }, 0x69);
    ip6_with_prefix(addrs[1], (const uint8_t[8]) {
        0x2a, 0x06, 0, 0, 0, 0, 0, 0
    }, 0x01);

    TEST_ASSERT_FALSE(mesh_scan_srp_addr_kept(addrs, 2, omr, 1, ML));
}

TEST_CASE("No addresses: mesh_scan_srp_addr_kept returns false", "[mesh_scan]")
{
    mesh_scan_prefix_t omr[1];

    memcpy(omr[0], OMR, 8);

    TEST_ASSERT_FALSE(mesh_scan_srp_addr_kept(NULL, 0, omr, 1, ML));
}

// ---- mesh_scan_txt_find ----

TEST_CASE("Key present with value: mesh_scan_txt_find returns true with value pointer and length", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, XA);
    const uint8_t *val = NULL;
    uint8_t val_len = 0;

    TEST_ASSERT_TRUE(mesh_scan_txt_find(txt, len, "xp", &val, &val_len));
    TEST_ASSERT_EQUAL_UINT8(8, val_len);
    TEST_ASSERT_EQUAL_MEMORY(XP, val, 8);
}

TEST_CASE("Key-only entry (at): mesh_scan_txt_find returns true with NULL value and length 0", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, XA);
    const uint8_t *val = (const uint8_t *)txt;
    uint8_t val_len = 9;

    TEST_ASSERT_TRUE(mesh_scan_txt_find(txt, len, "at", &val, &val_len));
    TEST_ASSERT_NULL(val);
    TEST_ASSERT_EQUAL_UINT8(0, val_len);
}

TEST_CASE("Key absent, or only a longer key with the same prefix: mesh_scan_txt_find returns false", "[mesh_scan]")
{
    uint8_t txt[64];
    uint16_t len = 0;
    const uint8_t *val;
    uint8_t val_len;

    len = txt_add_str(txt, len, "xpa=1");
    len = txt_add_str(txt, len, "nn=net");

    TEST_ASSERT_FALSE(mesh_scan_txt_find(txt, len, "xp", &val, &val_len));
    TEST_ASSERT_FALSE(mesh_scan_txt_find(txt, len, "xa", &val, &val_len));
}

TEST_CASE("A string length running past the buffer: mesh_scan_txt_find returns false without reading beyond len",
          "[mesh_scan]")
{
    // The last entry claims 9 bytes but the buffer ends after "xp=" + 2 bytes; bytes past len hold a valid "xp=".
    uint8_t buf[32] = {0};
    const uint8_t *val;
    uint8_t val_len;

    buf[0] = 9;
    memcpy(buf + 1, "xp=ab", 5);
    memcpy(buf + 6, "cdefgh", 6); // would complete the value if the length were trusted

    TEST_ASSERT_FALSE(mesh_scan_txt_find(buf, 6, "xp", &val, &val_len));
}

// ---- mesh_scan_txt_cut ----

TEST_CASE("TXT fits max: mesh_scan_txt_cut returns len unchanged", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, XA);

    TEST_ASSERT_EQUAL_UINT16(len, mesh_scan_txt_cut(txt, len, len));
    TEST_ASSERT_EQUAL_UINT16(len, mesh_scan_txt_cut(txt, len, 512));
}

TEST_CASE("TXT longer than max: mesh_scan_txt_cut returns the longest prefix of whole strings within max",
          "[mesh_scan]")
{
    uint8_t txt[64];
    uint16_t len = 0;

    len = txt_add_str(txt, len, "aaaa=1"); // 7 bytes
    len = txt_add_str(txt, len, "bbbb=2"); // 7 bytes, total 14
    len = txt_add_str(txt, len, "cccc=3"); // 7 bytes, total 21

    TEST_ASSERT_EQUAL_UINT16(14, mesh_scan_txt_cut(txt, len, 20));
    TEST_ASSERT_EQUAL_UINT16(14, mesh_scan_txt_cut(txt, len, 14));
    TEST_ASSERT_EQUAL_UINT16(7, mesh_scan_txt_cut(txt, len, 13));
}

TEST_CASE("First TXT string alone longer than max: mesh_scan_txt_cut returns 0", "[mesh_scan]")
{
    uint8_t txt[32];
    uint16_t len = txt_add_str(txt, 0, "aaaa=1");

    TEST_ASSERT_EQUAL_UINT16(0, mesh_scan_txt_cut(txt, len, 6));
}

// ---- mesh_scan_br_record_kept ----

TEST_CASE("_meshcop._udp with xp = this network, xa = another router: mesh_scan_br_record_kept returns true",
          "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, XA);

    TEST_ASSERT_TRUE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
    TEST_ASSERT_TRUE(mesh_scan_br_record_kept("_trel._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("xp of another network: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP_OTHER, XA);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("xa = this board's extended address: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, OWN_EXT);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("No xp: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, NULL, XA);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("xp = this network, no xa: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, NULL);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("xp = this network, xa of wrong length: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, NULL);

    len = txt_add(txt, len, "xa=\x01\x02\x03", 6);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_meshcop._udp", txt, len, XP, OWN_EXT));
}

TEST_CASE("Any other type: mesh_scan_br_record_kept returns false", "[mesh_scan]")
{
    uint8_t txt[128];
    uint16_t len = meshcop_txt(txt, XP, XA);

    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_matter._tcp", txt, len, XP, OWN_EXT));
    TEST_ASSERT_FALSE(mesh_scan_br_record_kept("_hap._udp", txt, len, XP, OWN_EXT));
}

// ---- mesh_scan_pool_put ----

TEST_CASE("Room left: mesh_scan_pool_put returns true, the offset and grows pool_used", "[mesh_scan]")
{
    static uint8_t pool[MESH_SCAN_POOL_SIZE];
    static mesh_scan_result_t r; // ~100 KB: not on the test stack
    uint16_t off = 0xffff;
    const uint8_t a[5] = {1, 2, 3, 4, 5};
    const uint8_t b[3] = {9, 8, 7};

    memset(&r, 0, sizeof(r));
    r.pool = pool;

    TEST_ASSERT_TRUE(mesh_scan_pool_put(&r, a, sizeof(a), &off));
    TEST_ASSERT_EQUAL_UINT16(0, off);
    TEST_ASSERT_EQUAL_UINT16(5, r.pool_used);
    TEST_ASSERT_TRUE(mesh_scan_pool_put(&r, b, sizeof(b), &off));
    TEST_ASSERT_EQUAL_UINT16(5, off);
    TEST_ASSERT_EQUAL_UINT16(8, r.pool_used);
    TEST_ASSERT_EQUAL_MEMORY(b, pool + 5, 3);
}

TEST_CASE("Would overflow 24 KiB: mesh_scan_pool_put returns false and leaves the pool unchanged", "[mesh_scan]")
{
    static uint8_t pool[MESH_SCAN_POOL_SIZE];
    static uint8_t data[MESH_SCAN_POOL_SIZE];
    static mesh_scan_result_t r; // ~100 KB: not on the test stack
    uint16_t off = 0x1234;

    memset(&r, 0, sizeof(r));
    r.pool = pool;
    r.pool_used = MESH_SCAN_POOL_SIZE - 4;

    TEST_ASSERT_FALSE(mesh_scan_pool_put(&r, data, 5, &off));
    TEST_ASSERT_EQUAL_UINT16(MESH_SCAN_POOL_SIZE - 4, r.pool_used);
    TEST_ASSERT_EQUAL_UINT16(0x1234, off);
    TEST_ASSERT_TRUE(mesh_scan_pool_put(&r, data, 4, &off)); // exactly full still fits
    TEST_ASSERT_EQUAL_UINT16(MESH_SCAN_POOL_SIZE, r.pool_used);
}

TEST_CASE("No pool allocated: mesh_scan_pool_put returns false", "[mesh_scan]")
{
    static mesh_scan_result_t r;
    uint16_t off = 0x1234;
    const uint8_t a[1] = {1};

    memset(&r, 0, sizeof(r));

    TEST_ASSERT_FALSE(mesh_scan_pool_put(&r, a, sizeof(a), &off));
    TEST_ASSERT_EQUAL_UINT16(0, r.pool_used);
    TEST_ASSERT_EQUAL_UINT16(0x1234, off);
}
