#pragma once

// A routing_ot_ops_t that records every call with its arguments, returns configurable results and answers the Network
// Data queries from a membership table. One global instance; fake_ops_reset() before each test.

#include <stdbool.h>
#include <stdint.h>

#include "publisher.h"

#define FAKE_MAX_CALLS 32
#define FAKE_MAX_NETDATA 4

typedef enum {
    FAKE_SET_OMR_CONFIG,
    FAKE_GET_OMR_PREFIX,
    FAKE_ADD_ON_MESH,
    FAKE_REMOVE_ON_MESH,
    FAKE_REGISTER_NETDATA,
    FAKE_PD_SET_ENABLED,
    FAKE_NETDATA_HAS_PREFIX,
    FAKE_DEFAULT_ROUTE_PUBLISHED,
    FAKE_OP_COUNT,
} fake_op_t;

typedef struct {
    fake_op_t op;
    routing_omr_mode_t mode; /**< set_omr_config */
    bool has_prefix;         /**< a prefix argument was given (not NULL) */
    routing_prefix64_t prefix;
    int8_t pref;   /**< set_omr_config, add_on_mesh */
    bool flag;     /**< add_on_mesh default_route, pd_set_enabled enabled */
} fake_call_t;

typedef struct {
    fake_call_t calls[FAKE_MAX_CALLS];
    int call_count;
    int ret[FAKE_OP_COUNT];        /**< result of each int op, 0 by default */
    routing_prefix64_t omr_prefix; /**< what get_omr_prefix returns */
    routing_prefix64_t netdata[FAKE_MAX_NETDATA];
    int netdata_count;
    bool default_route; /**< what default_route_published returns */
} fake_ops_state_t;

extern fake_ops_state_t g_fake;
extern const routing_ot_ops_t fake_ops;

void fake_ops_reset(void);
/** Forget the recorded calls, keep the configuration. */
void fake_ops_clear_calls(void);
/** Put a prefix into the Network Data table. */
void fake_ops_netdata_add(const routing_prefix64_t *prefix);
/** Calls of `op` recorded so far. */
int fake_ops_count(fake_op_t op);
/** The `nth` (0-based) recorded call of `op`, NULL when there is none. */
const fake_call_t *fake_ops_find(fake_op_t op, int nth);
/** Index in g_fake.calls of the `nth` call of `op`, -1 when there is none. */
int fake_ops_index(fake_op_t op, int nth);
/** Calls that change OpenThread state (everything but the queries and get_omr_prefix). */
int fake_ops_mutations(void);
