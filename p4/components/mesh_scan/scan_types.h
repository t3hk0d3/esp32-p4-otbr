#pragma once

// Result of one mesh scan. Pure data: filled by the collector, read by the renderer.

#include <stdbool.h>
#include <stdint.h>

#include "mesh_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_SCAN_MAX_ROUTERS 63 /**< indexed by router ID */
#define MESH_SCAN_MAX_CHILDREN 128
#define MESH_SCAN_MAX_ROUTER_ADDRS 16
#define MESH_SCAN_MAX_CHILD_ADDRS 8
#define MESH_SCAN_MAX_SRP_HOSTS 64
#define MESH_SCAN_MAX_SRP_ADDRS 8
#define MESH_SCAN_MAX_SRP_SERVICES 8
#define MESH_SCAN_MAX_SRP_SERVERS 8
#define MESH_SCAN_MAX_SUBTYPES 6        /**< per service */
#define MESH_SCAN_TXT_MAX 512           /**< TXT bytes kept per service */
#define MESH_SCAN_POOL_SIZE (24 * 1024) /**< TXT and subtype labels of one result */
#define MESH_SCAN_NAME_LEN 64
#define MESH_SCAN_MAX_OMR 4
#define MESH_SCAN_SRP_QUERIES 4        /**< DNS queries in flight at most */
#define MESH_SCAN_SRP_SERVER_QUERIES 2 /**< ... to one server, when more than one remote server is asked */
#define MESH_SCAN_SRP_RESOLVES 48      /**< service resolves per scan */
#define MESH_SCAN_SRP_NAME_RESOLVES 16 /**< ... of them kept for _esphomelib._tcp and _hap._udp */
#define MESH_SCAN_SRP_BUDGET_MS 15000  /**< SRP step budget from scan start, inside the cap */
#define MESH_SCAN_DNS_TIMEOUT_MS 3000  /**< DNS response timeout per attempt */
#define MESH_SCAN_DNS_ATTEMPTS 2

/** An IPv6 address or prefix, network byte order. */
typedef uint8_t mesh_scan_ip6_t[16];
/** The first 64 bits of an address. */
typedef uint8_t mesh_scan_prefix_t[8];

/** What became of the device-info query to one router or child. Zero means not asked. */
typedef enum {
    MESH_SCAN_INFO_NOT_ASKED = 0, /**< never asked: a router that did not answer discover, or a zeroed entry */
    MESH_SCAN_INFO_ANSWERED,      /**< any reply from the device; the fields it did not carry stay empty */
    MESH_SCAN_INFO_NO_ANSWER,     /**< asked, the deadline passed or the send failed */
    MESH_SCAN_INFO_CUT_SHORT,     /**< still queued when a cap hit */
} mesh_scan_info_state_t;

/** Device info from the Network Diagnostic TLVs 23, 25-28, 35 and 44. Empty strings mean absent. */
typedef struct {
    mesh_scan_info_state_t state;
    bool has_eui64;
    uint8_t eui64[8];
    bool has_oui;
    uint32_t oui; /**< 24 bits */
    char vendor_name[33];
    char vendor_model[33];
    char vendor_sw_version[17];
    char thread_stack_version[65];
    char vendor_app_url[97];
} mesh_scan_info_t;

typedef struct {
    uint16_t rloc16;
    uint8_t ext[8];
    bool has_ext;
    bool rx_on_when_idle;
    bool full_thread_device;
    bool full_network_data;
    uint8_t lq_in;
    uint32_t timeout_s;
    uint32_t age_s;
    uint32_t connected_s;
    int8_t rssi_avg;
    int8_t rssi_last;
    uint8_t link_margin_db;
    bool has_err_rate;  /**< the parent tracks the error rates; frame_err and msg_err are meaningless otherwise */
    uint16_t frame_err; /**< raw 0..0xffff, 0xffff = 100 % */
    uint16_t msg_err;   /**< raw 0..0xffff */
    uint16_t version;
    bool csl;
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_CHILD_ADDRS];
    uint8_t n_addrs;
    bool addresses_reported; /**< false: asked directly (spec 3.4) and gave no answer; true otherwise */
    bool ask_pending;        /**< internal to scan.c: queued for the info phase, not asked yet */
    mesh_scan_info_t info;
} mesh_scan_child_t;

typedef struct {
    bool present;
    bool responded; /**< false: stub from the local router table, only router_id and rloc16 are valid */
    uint16_t rloc16;
    uint8_t router_id;
    uint8_t ext[8];
    bool has_version; /**< the router reported its Thread version */
    uint16_t version;
    bool leader;
    bool this_device;
    bool border_router;
    uint8_t lq_in[MESH_SCAN_MAX_ROUTERS]; /**< incoming LQ from each router ID, 0 = no link */
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_ROUTER_ADDRS];
    uint8_t n_addrs;
    bool children_detail;
    mesh_scan_child_t *children; /**< n_children entries allocated by scan.c through ops.alloc; NULL when 0 */
    uint16_t n_children;
    mesh_scan_info_t info;
} mesh_scan_router_t;

typedef enum {
    MESH_SCAN_SRP_ANSWERED,
    MESH_SCAN_SRP_NO_ANSWER,
    MESH_SCAN_SRP_CUT_SHORT,
} mesh_scan_srp_state_t;

/** A DNS/SRP unicast server found in the network data. */
typedef struct {
    uint16_t rloc16;
    mesh_scan_ip6_t addr;
    uint16_t port;
    bool this_board;
    mesh_scan_srp_state_t state;
    uint8_t hosts;
} mesh_scan_srp_server_t;

typedef struct {
    char name[MESH_SCAN_NAME_LEN];
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_SRP_ADDRS];
    uint8_t n_addrs;
    struct {
        char instance[MESH_SCAN_NAME_LEN];
        char type[MESH_SCAN_NAME_LEN];
        uint16_t port;
        uint16_t source;      /**< RLOC16 of the server this record came from */
        uint16_t txt_off;     /**< TXT bytes in the result's pool */
        uint16_t txt_len;
        bool has_txt;         /**< false: no TXT known (txt_len is 0) */
        bool txt_truncated;   /**< cut at the TXT cap or left out because the pool was full */
        uint16_t sub_off;     /**< subtype labels in the pool, each followed by a NUL */
        uint8_t n_sub;
    } services[MESH_SCAN_MAX_SRP_SERVICES];
    uint8_t n_services;
    uint16_t source;          /**< RLOC16 of the server the host fields came from */
    bool local;               /**< from this board's SRP server: lease_s and key_lease_s are valid, ttl_s is not */
    uint32_t lease_s;         /**< remaining lease, local only */
    uint32_t key_lease_s;     /**< remaining key lease, local only */
    uint32_t ttl_s;           /**< remote only */
} mesh_scan_srp_host_t;

typedef struct {
    char name[MESH_SCAN_NAME_LEN];
    uint8_t channel;
    uint32_t partition_id;
    uint8_t leader_router_id;
    mesh_scan_prefix_t ml_prefix;
    uint16_t this_rloc16;
    mesh_scan_prefix_t omr[MESH_SCAN_MAX_OMR];
    uint8_t n_omr;
} mesh_scan_network_t;

/**
 * About 106 KB (the SRP hosts 64 x ~1.3 KB = 84 KB, the routers 22 KB): allocated by scan.c through `ops.alloc`
 * (PSRAM on target), never on a stack. Up to two can exist at once: a new scan allocates its result while a render
 * still holds the old one.
 */
#define MESH_SCAN_PREF_NONE (-1) /**< the neighbour does not use that radio */

/** How this board reaches one router neighbour (OpenThread multi-radio: 15.4 and TREL). */
typedef struct {
    bool valid;        /**< this board has a link to that router and OpenThread knows its radios */
    uint8_t lq_in;     /**< this board's incoming link quality from that router, as its own router table has it */
    int16_t pref_154;  /**< OpenThread's 15.4 preference 0..255, MESH_SCAN_PREF_NONE when not used */
    int16_t pref_trel; /**< OpenThread's TREL preference 0..255, MESH_SCAN_PREF_NONE when not used */
} mesh_scan_transport_t;

typedef struct {
    mesh_scan_network_t network;
    mesh_scan_router_t routers[MESH_SCAN_MAX_ROUTERS]; /**< by router ID; absent entries have present == false */
    bool has_transport; /**< the firmware has TREL: this board's links carry this_links */
    mesh_scan_transport_t this_links[MESH_SCAN_MAX_ROUTERS]; /**< by router ID, filled by the snapshot */
    mesh_scan_srp_host_t srp[MESH_SCAN_MAX_SRP_HOSTS];
    uint8_t n_srp;
    mesh_scan_srp_server_t srp_servers[MESH_SCAN_MAX_SRP_SERVERS];
    uint8_t n_srp_servers;
    uint8_t *pool;      /**< MESH_SCAN_POOL_SIZE bytes, a second ops.alloc block owned by the result */
    uint16_t pool_used;
    bool partial;
} mesh_scan_result_t;

/** What the renderer needs: a consistent copy of the state, valid for the duration of one render. */
typedef struct {
    mesh_scan_state_t state;
    mesh_scan_error_t error; /**< failed only */
    uint8_t routers;         /**< running: routers discovered, 0 until discover ends */
    uint8_t queried;         /**< running: routers finished */
    uint16_t children_total;   /**< running: rx-on children asked for their addresses, 0 before that */
    uint16_t children_queried; /**< running: of those, answered or given up */
    uint16_t info_total;       /**< running: devices to ask for their info, 0 before the info phase */
    uint16_t info_answered;    /**< running: of those, answered */
    uint16_t sleepy_waiting;   /**< running: sleepy devices still waiting for an answer */
    uint16_t sleepy_wait_s;    /**< running: seconds until the latest sleepy deadline, 0 when none waits */
    uint32_t age_s;          /**< done and failed: seconds since the scan ended */
    const mesh_scan_result_t *result; /**< done only, non-NULL then */
} mesh_scan_view_t;

#ifdef __cplusplus
}
#endif
