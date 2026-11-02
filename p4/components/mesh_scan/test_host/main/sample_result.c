#include <arpa/inet.h>
#include <string.h>

#include "sample_result.h"

static mesh_scan_result_t s_result;
static mesh_scan_child_t s_children[4][4];

static void ip6(mesh_scan_ip6_t out, const char *text)
{
    inet_pton(AF_INET6, text, out);
}

static void prefix(mesh_scan_prefix_t out, const char *text)
{
    mesh_scan_ip6_t full;

    ip6(full, text);
    memcpy(out, full, 8);
}

static void ext(uint8_t out[8], uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(value >> (56 - 8 * i));
    }
}

static mesh_scan_router_t *router(uint8_t id, uint16_t rloc16, uint64_t ext_addr)
{
    mesh_scan_router_t *r = &s_result.routers[id];

    r->present = true;
    r->responded = true;
    r->router_id = id;
    r->rloc16 = rloc16;
    r->version = 5;
    r->children_detail = true;
    ext(r->ext, ext_addr);
    return r;
}

static void child(mesh_scan_router_t *r, uint16_t rloc16, uint64_t ext_addr, bool sleepy, const char *addr)
{
    mesh_scan_child_t *c = &r->children[r->n_children++];

    c->rloc16 = rloc16;
    c->has_ext = true;
    ext(c->ext, ext_addr);
    c->rx_on_when_idle = !sleepy;
    c->full_thread_device = !sleepy;
    c->full_network_data = !sleepy;
    c->lq_in = 3;
    c->timeout_s = 240;
    c->age_s = 12;
    c->connected_s = 86400;
    c->rssi_avg = -61;
    c->rssi_last = -63;
    c->link_margin_db = 39;
    c->frame_err = 0x0100; // 0.39 %
    c->msg_err = 0;
    c->version = 4;
    c->csl = false;
    ip6(c->addrs[0], addr);
    c->n_addrs = 1;
}

mesh_scan_result_t *sample_result(void)
{
    memset(&s_result, 0, sizeof(s_result));
    memset(s_children, 0, sizeof(s_children));

    mesh_scan_network_t *n = &s_result.network;

    strcpy(n->name, "MyThread");
    n->channel = 15;
    n->partition_id = 4026531841u; // above INT32_MAX
    n->leader_router_id = 9;
    prefix(n->ml_prefix, "fdde:ad00:beef:0::");
    n->this_rloc16 = 0x8800;
    prefix(n->omr[0], "2001:db8:4a2:1::");
    prefix(n->omr[1], "fd7a:91c3:22e0:1::");
    n->n_omr = 2;

    mesh_scan_router_t *leader = router(9, 0x2400, 0xd61a7c03f4e2b918ull);
    mesh_scan_router_t *board = router(34, 0x8800, 0x0a1b2c3d4e5f6071ull);
    mesh_scan_router_t *r3 = router(3, 0x0c00, 0x00000000000000a1ull);
    mesh_scan_router_t *r17 = router(17, 0x4400, 0x11223344556677ffull);
    mesh_scan_router_t *r50 = router(50, 0xc800, 0xfedcba9876543210ull);
    mesh_scan_router_t *stub = &s_result.routers[25];

    stub->present = true;
    stub->responded = false;
    stub->router_id = 25;
    stub->rloc16 = 0x6400;

    leader->leader = true;
    board->this_device = true;
    board->border_router = true;
    leader->children = s_children[0];
    board->children = s_children[1];
    r3->children = s_children[2];
    r17->children = s_children[3];

    leader->lq_in[34] = 3;
    leader->lq_in[3] = 2;
    leader->lq_in[17] = 0; // no link: left out
    leader->lq_in[25] = 1; // links may point at the stub
    board->lq_in[9] = 3;
    board->lq_in[50] = 1;
    r3->lq_in[9] = 2;
    r17->lq_in[34] = 2;
    r50->lq_in[34] = 1;

    ip6(leader->addrs[0], "fdde:ad00:beef:0:0:ff:fe00:2400");
    ip6(leader->addrs[1], "fdde:ad00:beef:0:1c2d:3e4f:5a6b:7c8d");
    leader->n_addrs = 2;
    ip6(board->addrs[0], "fdde:ad00:beef:0:0:ff:fe00:8800");
    board->n_addrs = 1;
    ip6(r3->addrs[0], "fdde:ad00:beef:0:0:ff:fe00:c00");
    r3->n_addrs = 1;
    ip6(r17->addrs[0], "fdde:ad00:beef:0:0:ff:fe00:4400");
    r17->n_addrs = 1;
    ip6(r50->addrs[0], "fdde:ad00:beef:0:0:ff:fe00:c800");
    r50->n_addrs = 1;

    child(leader, 0x2401, 0x1c88f02ab4d6e5a3ull, true, "2001:db8:4a2:1:2be4:6c19:a0f7:3381");
    child(leader, 0x2402, 0x1c88f02ab4d6e5a4ull, false, "2001:db8:4a2:1::2");
    child(leader, 0x2403, 0x1c88f02ab4d6e5a5ull, true, "fd7a:91c3:22e0:1::3");
    child(board, 0x8801, 0x2000000000000001ull, true, "2001:db8:4a2:1::8801");
    child(board, 0x8802, 0x2000000000000002ull, true, "2001:db8:4a2:1::8802");
    child(board, 0x8803, 0x2000000000000003ull, false, "2001:db8:4a2:1::8803");
    child(r3, 0x0c01, 0x3000000000000001ull, true, "2001:db8:4a2:1::c01");
    child(r3, 0x0c02, 0x3000000000000002ull, true, "2001:db8:4a2:1::c02");
    r17->children_detail = false;
    child(r17, 0x4401, 0x4000000000000001ull, true, "2001:db8:4a2:1::4401");

    mesh_scan_srp_host_t *h = &s_result.srp[0];

    strcpy(h->name, "kitchen-plug");
    ip6(h->addrs[0], "2001:db8:4a2:1:2be4:6c19:a0f7:3381");
    h->n_addrs = 1;
    strcpy(h->services[0].instance, "kitchen-plug");
    strcpy(h->services[0].type, "_matter._tcp");
    h->n_services = 1;

    h = &s_result.srp[1];
    strcpy(h->name, "a\"b\\<c>");
    ip6(h->addrs[0], "fd7a:91c3:22e0:1::3");
    ip6(h->addrs[1], "::");
    h->n_addrs = 2;
    h->n_services = 0;
    s_result.n_srp = 2;

    s_result.partial = true; // router 25 did not answer, router 17 has no detail
    return &s_result;
}
