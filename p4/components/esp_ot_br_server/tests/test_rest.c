/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "host_platform.h"
#define ESP_OT_REST_API_VERSION "2.0.0"
#include "rest_api.inc"
#include "rest_web.inc"

static httpd_req_t request(httpd_method_t method, const char *uri, const char *body, const char *accept,
                           const char *type)
{
    httpd_req_t req = {
        .method = method, .uri = uri, .input = body ? body : "", .content_len = body ? strlen(body) : 0, .fragment = 3};
    req.headers = cJSON_CreateObject();
    if (accept) {
        cJSON_AddStringToObject(req.headers, "aCcEpT", accept);
    }
    if (type) {
        cJSON_AddStringToObject(req.headers, "content-type", type);
    }
    return req;
}
static cJSON *run_request(httpd_req_t *req, int status)
{
    esp_otbr_thread_rest_handler(req);
    assert(req->status && atoi(req->status) == status);
    assert(!strcmp(cJSON_GetStringValue(rest_get(req->sent_headers, "Access-Control-Allow-Origin")), "*"));
    return cJSON_Parse(req->body);
}
static void release(httpd_req_t *req, cJSON *json)
{
    cJSON_Delete(req->headers);
    cJSON_Delete(req->sent_headers);
    cJSON_Delete(json);
    free(req->body);
    free(req->status);
    free(req->type);
}
static void test_node(void)
{
    httpd_req_t req = request(HTTP_GET, "/node", NULL, NULL, NULL);
    cJSON *json = run_request(&req, 200);
    assert(!strcmp(rest_string(json, "state"), "disabled"));
    assert(rest_get(json, "leaderData") && !rest_get(json, "LeaderData"));
    assert(!strcmp(rest_string(json, "rloc16"), "0x0000"));
    assert(!strcmp(rest_string(json, "rlocAddress"), "::"));
    assert(rest_get(json, "routerCount")->valueint == 1);
    release(&req, json);
    req = request(HTTP_GET, "/node/coprocessor/version", NULL, NULL, NULL);
    json = run_request(&req, 200);
    assert(!strcmp(cJSON_GetStringValue(json), "test-rcp"));
    release(&req, json);
    req = request(HTTP_GET, "/.well-known/thread/br-rest", NULL, NULL, NULL);
    json = run_request(&req, 200);
    assert(!strcmp(rest_string(rest_get(json, "api"), "version"), ESP_OT_REST_API_VERSION));
    release(&req, json);
}
static void test_routing(void)
{
    httpd_req_t req = request(HTTP_OPTIONS, "/node/state", NULL, NULL, NULL);
    cJSON *json = run_request(&req, 200);
    assert(!strcmp(rest_string(req.sent_headers, "Allow"), "GET, PUT, OPTIONS"));
    release(&req, json);
    req = request(HTTP_PATCH, "/node/state", NULL, NULL, NULL);
    json = run_request(&req, 405);
    assert(rest_get(json, "status")->valueint == 405);
    release(&req, json);
    req = request(HTTP_GET, "/node/unknown", NULL, NULL, NULL);
    json = run_request(&req, 404);
    release(&req, json);
    req = request(HTTP_GET, "/node/%00", NULL, NULL, NULL);
    json = run_request(&req, 400);
    release(&req, json);
    req = request(HTTP_GET, "/api/actions?fields%5BupdateDeviceCollectionTask%5D=%GG", NULL, "*/*", NULL);
    json = run_request(&req, 400);
    release(&req, json);
    req = request(HTTP_POST, "/api/actions", NULL, NULL, "application/vnd.api+json");
    req.content_len = 16385;
    json = run_request(&req, 413);
    assert(!strcmp(rest_string(req.sent_headers, "Connection"), "close"));
    release(&req, json);
    req = request(HTTP_PUT, "/node/state", "\"enable\"", NULL, "application/json");
    req.content_len += 3;
    json = run_request(&req, 400);
    release(&req, json);
}
static void test_route_matrix(void)
{
    unsigned operations = 0;
    for (unsigned i = 0; i < sizeof(s_thread_rest_routes) / sizeof(s_thread_rest_routes[0]); i++) {
        const thread_rest_route_t *route = &s_thread_rest_routes[i];
        operations += __builtin_popcount(route->methods);
        char uri[128];
        snprintf(uri, sizeof(uri), "%s%s", route->path, route->item ? "item" : "");
        httpd_req_t req = request(HTTP_OPTIONS, uri, NULL, NULL, NULL);
        cJSON *json = run_request(&req, 200);
        assert(strstr(rest_string(req.sent_headers, "Allow"), "OPTIONS"));
        release(&req, json);
    }
    assert(operations == 42);
}

static void test_commissioner_and_energy(void)
{
    httpd_req_t req = request(HTTP_PUT, "/node/commissioner/state", "\"enable\"", NULL, "application/json");
    cJSON *json = run_request(&req, 200);
    release(&req, json);
    req = request(HTTP_POST, "/node/commissioner/joiner", "{\"pskd\":\"ABC123\",\"joinerId\":\"*\"}", NULL,
                  "application/json");
    json = run_request(&req, 200);
    release(&req, json);
    const char *body =
        "{\"data\":[{\"type\":\"addThreadDeviceTask\",\"attributes\":{\"pskd\":\"ABC123\",\"joinerId\":\"*\"}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    assert(rest_get(rest_get(cJSON_GetArrayItem(rest_get(json, "data"), 0), "attributes"), "timeout")->valueint == 60);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    thread_rest_item_t *action = s_rest_actions;
    assert(action->joiner_registered);
    rest_joiner_callback(OT_COMMISSIONER_JOINER_START, &action->joiner, NULL, NULL);
    assert(!strcmp(rest_string(action->attributes, "status"), "attempted"));
    rest_joiner_callback(OT_COMMISSIONER_JOINER_END, &action->joiner, NULL, NULL);
    rest_poll(esp_openthread_get_instance());
    assert(action->complete && !action->joiner_registered);
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
    body = "{\"data\":[{\"type\":\"getEnergyScanTask\",\"attributes\":{\"destination\":\"0x0000\",\"channelMask\":[11,"
           "12],\"count\":1,\"period\":100,\"scanDuration\":10,\"timeout\":60}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    action = s_rest_energy_action;
    assert(action);
    const uint8_t energy[] = {156, 166, 176, 186};
    rest_energy_callback((1u << 11) | (1u << 12), energy, 4, (void *)(uintptr_t)action->token);
    assert(action->complete && !s_rest_energy_action);
    cJSON *report = rest_get(s_rest_diagnostics->attributes, "report");
    assert(cJSON_GetArraySize(report) == 2);
    cJSON *rssi = rest_get(cJSON_GetArrayItem(report, 0), "maxRssi");
    assert(cJSON_GetArrayItem(rssi, 0)->valueint == -100);
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
    body = "{\"data\":[{\"type\":\"resetNetworkDiagCounterTask\",\"attributes\":{\"types\":[\"macCounters\"],"
           "\"timeout\":5}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    assert(s_rest_actions->complete);
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
}

static void test_dataset(void)
{
    cJSON *json = cJSON_Parse("{\"networkName\":\"mesh\",\"networkKey\":\"00112233445566778899aabbccddeeff\","
                              "\"channel\":15,\"activeTimestamp\":{\"seconds\":42}}");
    otOperationalDataset dataset = {0};
    assert(rest_parse_dataset(json, &dataset, false));
    assert(!strcmp(dataset.mNetworkName.m8, "mesh") && dataset.mChannel == 15 &&
           dataset.mActiveTimestamp.mSeconds == 42);
    cJSON_Delete(json);
    json = cJSON_Parse("{\"networkKey\":\"abc\"}");
    assert(!rest_parse_dataset(json, &dataset, false));
    cJSON_Delete(json);
    json = cJSON_Parse("{\"activeDataset\":{},\"delay\":30000}");
    assert(rest_parse_dataset(json, &dataset, true));
    cJSON_Delete(json);
    json = cJSON_Parse("{\"delay\":30000}");
    assert(!rest_parse_dataset(json, &dataset, true));
    cJSON_Delete(json);
    httpd_req_t req = request(HTTP_GET, "/node/dataset/active", NULL, NULL, NULL);
    json = run_request(&req, 200);
    assert(!strcmp(rest_string(json, "networkName"), "test-mesh"));
    release(&req, json);
    req = request(HTTP_GET, "/node/dataset/active", NULL, "text/plain", NULL);
    json = run_request(&req, 200);
    assert(!strcmp(req.body, "ABCD") && !strcmp(req.type, "text/plain"));
    release(&req, json);
    req = request(HTTP_PUT, "/node/dataset/active", "{}", NULL, "application/json");
    cJSON_AddStringToObject(req.headers, "If-None-Match", " * ");
    json = run_request(&req, 412);
    release(&req, json);
    mock_role = OT_DEVICE_ROLE_CHILD;
    req =
        request(HTTP_PUT, "/node/dataset/pending", "{\"activeDataset\":{},\"delay\":30000}", NULL, "application/json");
    json = run_request(&req, 201);
    release(&req, json);
    mock_pending_result = OT_ERROR_REJECTED;
    req =
        request(HTTP_PUT, "/node/dataset/pending", "{\"activeDataset\":{},\"delay\":30000}", NULL, "application/json");
    json = run_request(&req, 409);
    assert(!strcmp(rest_string(json, "detail"), "rejected by leader"));
    release(&req, json);
    mock_pending_result = OT_ERROR_RESPONSE_TIMEOUT;
    req =
        request(HTTP_PUT, "/node/dataset/pending", "{\"activeDataset\":{},\"delay\":30000}", NULL, "application/json");
    json = run_request(&req, 504);
    release(&req, json);
    mock_role = OT_DEVICE_ROLE_DISABLED;
}
#define GUARD_DATASET                                                                                     \
    "{\"networkName\":\"mesh\",\"networkKey\":\"00112233445566778899aabbccddeeff\",\"channel\":15," \
    "\"activeTimestamp\":{\"seconds\":42}}"
/* Patch 14: PUT /node/dataset/active while Thread runs stops Thread, stores the dataset and starts Thread again. */
static const uint8_t s_new_tlvs[] = {0x01, 0x02, 0x03};
static void dataset_case(otDeviceRole role, bool changes)
{
    mock_role = role;
    mock_ot_writes = 0;
    mock_calls[0] = 0;
    mock_dataset_error = OT_ERROR_NONE;
    mock_set_active_error = OT_ERROR_NONE;
    mock_thread_enable_error = OT_ERROR_NONE;
    memset(&mock_active_tlvs, 0, sizeof(mock_active_tlvs));
    mock_active_tlvs.mLength = 2;
    mock_active_tlvs.mTlvs[0] = 0xab;
    mock_active_tlvs.mTlvs[1] = 0xcd;
    memset(&mock_update_tlvs, 0, sizeof(mock_update_tlvs));
    if (changes) {
        mock_update_tlvs.mLength = sizeof(s_new_tlvs);
        memcpy(mock_update_tlvs.mTlvs, s_new_tlvs, sizeof(s_new_tlvs));
    }
}
static bool stored_is_new(void)
{
    return mock_active_tlvs.mLength == sizeof(s_new_tlvs) && !memcmp(mock_active_tlvs.mTlvs, s_new_tlvs, sizeof(s_new_tlvs));
}
static bool stored_is_old(void)
{
    return mock_active_tlvs.mLength == 2 && mock_active_tlvs.mTlvs[0] == 0xab && mock_active_tlvs.mTlvs[1] == 0xcd;
}
static void put_active(const char *body, const char *type, int status)
{
    httpd_req_t req = request(HTTP_PUT, "/node/dataset/active", body, NULL, type);
    cJSON *json = run_request(&req, status);
    release(&req, json);
}
static void test_dataset_active_while_running(void)
{
    /* Thread disabled, no dataset stored: stored, 201, Thread is not touched. */
    dataset_case(OT_DEVICE_ROLE_DISABLED, true);
    mock_dataset_error = OT_ERROR_NOT_FOUND;
    put_active("010203", "text/plain", 201);
    assert(!strcmp(mock_calls, "S") && stored_is_new());
    /* Thread disabled, dataset stored: stored, 200, Thread is not touched. */
    dataset_case(OT_DEVICE_ROLE_DISABLED, true);
    put_active("010203", "text/plain", 200);
    assert(!strcmp(mock_calls, "S") && stored_is_new());
    /* Thread attached (any role but disabled), different TLVs: stop, store, start; 200 instead of 409. */
    static const otDeviceRole running[] = {OT_DEVICE_ROLE_DETACHED, OT_DEVICE_ROLE_CHILD, OT_DEVICE_ROLE_ROUTER,
                                           OT_DEVICE_ROLE_LEADER
                                          };
    for (unsigned i = 0; i < sizeof(running) / sizeof(running[0]); i++) {
        dataset_case(running[i], true);
        put_active("010203", "text/plain", 200);
        assert(!strcmp(mock_calls, "DSE") && stored_is_new());
    }
    /* Thread attached, TLVs equal to the stored ones: 200 without any OpenThread write. */
    dataset_case(OT_DEVICE_ROLE_LEADER, false);
    put_active("abcd", "text/plain", 200);
    assert(mock_ot_writes == 0 && !mock_calls[0] && mock_role == OT_DEVICE_ROLE_LEADER && stored_is_old());
    /* Thread attached, the store fails: Thread is started again on the old dataset, 500. */
    dataset_case(OT_DEVICE_ROLE_LEADER, true);
    mock_set_active_error = OT_ERROR_FAILED;
    put_active("010203", "text/plain", 500);
    assert(!strcmp(mock_calls, "DSE") && stored_is_old());
    /* Thread attached, the restart fails: the new dataset stays stored, 500. */
    dataset_case(OT_DEVICE_ROLE_LEADER, true);
    mock_thread_enable_error = OT_ERROR_FAILED;
    put_active("010203", "text/plain", 500);
    assert(!strcmp(mock_calls, "DSE") && stored_is_new());
    /* If-None-Match: * with a stored dataset while attached: 412, nothing written. */
    dataset_case(OT_DEVICE_ROLE_LEADER, true);
    httpd_req_t req = request(HTTP_PUT, "/node/dataset/active", "010203", NULL, "text/plain");
    cJSON_AddStringToObject(req.headers, "If-None-Match", "*");
    cJSON *json = run_request(&req, 412);
    release(&req, json);
    assert(mock_ot_writes == 0 && stored_is_old());
    /* A JSON body (create_active_dataset) while attached takes the same path. */
    dataset_case(OT_DEVICE_ROLE_ROUTER, true);
    put_active(GUARD_DATASET, "application/json", 200);
    assert(!strcmp(mock_calls, "DSE") && stored_is_new());
    dataset_case(OT_DEVICE_ROLE_DISABLED, false);
}
static void test_actions(void)
{
    httpd_req_t req = request(HTTP_GET, "/api/actions", NULL, NULL, NULL);
    cJSON *json = run_request(&req, 406);
    release(&req, json);
    req = request(HTTP_POST, "/api/actions", "{\"data\":[]}", NULL, "application/json");
    json = run_request(&req, 415);
    release(&req, json);
    req = request(HTTP_POST, "/api/actions", "{\"data\":[{\"type\":\"unknown\",\"attributes\":{}}]}", NULL,
                  THREAD_REST_JSON_API);
    json = run_request(&req, 422);
    release(&req, json);
    const char *body = "{\"data\":[{\"type\":\"updateDeviceCollectionTask\",\"attributes\":{\"maxAge\":1.5,"
                       "\"maxRetries\":3,\"deviceCount\":5,\"timeout\":5}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    cJSON *item = cJSON_GetArrayItem(rest_get(json, "data"), 0);
    char id[37];
    strlcpy(id, rest_string(item, "id"), sizeof(id));
    assert(strlen(id) == 36);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    assert(s_rest_mesh_action && s_rest_mesh_action->active);
    char uri[160];
    snprintf(uri, sizeof(uri), "/api/actions/%s?fields%%5BupdateDeviceCollectionTask%%5D=status", id);
    req = request(HTTP_GET, uri, NULL, THREAD_REST_JSON_API, NULL);
    json = run_request(&req, 200);
    item = rest_get(rest_get(json, "data"), "attributes");
    assert(rest_get(item, "status") && !rest_get(item, "maxAge"));
    release(&req, json);
    snprintf(uri, sizeof(uri), "/api/actions/%s", id);
    req = request(HTTP_DELETE, uri, NULL, NULL, NULL);
    json = run_request(&req, 204);
    release(&req, json);
    assert(!s_rest_mesh_action);
    body = "{\"data\":[{\"type\":\"getNetworkDiagnosticTask\",\"attributes\":{\"destination\":\"0x0000\",\"types\":["
           "\"children\"],\"timeout\":60}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    rest_poll(esp_openthread_get_instance());
    assert(s_rest_query_action);
    otMeshDiagChildEntry child = {.mRloc16 = 1, .mFrameErrorRate = 65535, .mSupportsErrRate = true};
    rest_children_callback(OT_ERROR_NONE, &child, (void *)(uintptr_t)s_rest_query_action->token);
    rest_poll(esp_openthread_get_instance());
    assert(s_rest_diagnostics && s_rest_actions->complete);
    cJSON *children = rest_get(s_rest_diagnostics->attributes, "children");
    assert(cJSON_GetArraySize(children) == 1);
    assert(rest_get(cJSON_GetArrayItem(children, 0), "frameErrorRate")->valuedouble == 1);
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
    body = "{\"data\":[{\"type\":\"getNetworkDiagnosticTask\",\"attributes\":{\"destination\":\"0x0000\",\"types\":["
           "\"extAddress\"],\"timeout\":60}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    uint32_t old_token = s_rest_actions->token;
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    rest_poll(esp_openthread_get_instance());
    mock_deliver_diagnostic(old_token);
    assert(!s_rest_actions->complete);
    mock_deliver_diagnostic(0);
    assert(s_rest_actions->complete);
}
/* Patches 2 and 9: the mode guard of the Thread REST API. */
static cJSON *guard_request(httpd_req_t *req, httpd_method_t method, const char *uri, const char *body, int status,
                            const char *detail)
{
    *req = request(method, uri, body, NULL, body ? "application/json" : NULL);
    cJSON *json = run_request(req, status);
    if (detail) {
        assert(rest_get(json, "status")->valueint == status);
        assert(!strcmp(rest_string(json, "detail"), detail));
        assert(!strcmp(rest_string(json, "title"), status == 409 ? "Conflict" : "Service Unavailable"));
    }
    return json;
}
static void guard_enter_mode(br_core_mode_t mode, bool ready)
{
    mock_mode = mode;
    mock_ot_ready = ready;
    mock_ot_writes = 0;
}
static void guard_flip_to_sniffer(void)
{
    mock_ot_ready = false;
    mock_mode = BR_MODE_SNIFFER;
}
static void test_guard_sniffer(void)
{
    static const struct {
        httpd_method_t method;
        const char *uri;
        const char *body;
    } writes[] = {
        {HTTP_PUT, "/node/dataset/active", GUARD_DATASET},
        {HTTP_PUT, "/node/dataset/pending", "{\"activeDataset\":{},\"delay\":30000}"},
        {HTTP_PUT, "/node/state", "\"enable\""},
        {HTTP_PUT, "/node/commissioner/state", "\"enable\""},
        {HTTP_PUT, "/node/ba-epskc/state", "\"enable\""},
        {HTTP_POST, "/node/ba-epskc/key", ""},
        {HTTP_POST, "/api/actions", ""},
        {HTTP_DELETE, "/node", NULL},
    };
    guard_enter_mode(BR_MODE_SNIFFER, false);
    for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        httpd_req_t req;
        cJSON *json = guard_request(&req, writes[i].method, writes[i].uri, writes[i].body, 409, "sniffer active");
        release(&req, json);
    }
    assert(mock_ot_writes == 0);
    httpd_req_t req = request(HTTP_GET, "/node/state", NULL, NULL, NULL);
    cJSON *json = run_request(&req, 503); /* Thread REST reads are off in the sniffer (OpenThread switched off) */
    release(&req, json);
    req = request(HTTP_GET, "/.well-known/thread/br-rest", NULL, NULL, NULL);
    json = run_request(&req, 200);
    release(&req, json);
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
}
static void test_guard_radio_fault(void)
{
    guard_enter_mode(BR_MODE_RADIO_FAULT, false);
    httpd_req_t req;
    cJSON *json = guard_request(&req, HTTP_PUT, "/node/dataset/active", GUARD_DATASET, 503, "radio not available");
    release(&req, json);
    json = guard_request(&req, HTTP_GET, "/node/state", NULL, 503, "radio not available");
    release(&req, json);
    assert(mock_ot_writes == 0);
    req = request(HTTP_GET, "/.well-known/thread/br-rest", NULL, NULL, NULL);
    json = run_request(&req, 200);
    release(&req, json);
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
}
static void test_guard_br_running(void)
{
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
    httpd_req_t req = request(HTTP_PUT, "/node/state", "\"enable\"", NULL, "application/json");
    cJSON *json = run_request(&req, 200);
    assert(mock_ot_writes > 0);
    release(&req, json);
    req = request(HTTP_PUT, "/node/state", "\"disable\"", NULL, "application/json");
    json = run_request(&req, 200);
    release(&req, json);
    req = request(HTTP_GET, "/node/state", NULL, NULL, NULL);
    json = run_request(&req, 200);
    release(&req, json);
}
static void test_guard_mode_change_while_waiting_for_the_lock(void)
{
    /* The sniffer starts after the pre-check and before the handler holds the OpenThread lock. */
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
    mock_on_lock = guard_flip_to_sniffer;
    httpd_req_t req;
    cJSON *json = guard_request(&req, HTTP_PUT, "/node/dataset/active", GUARD_DATASET, 409, "sniffer active");
    release(&req, json);
    assert(mock_mode == BR_MODE_SNIFFER && mock_ot_writes == 0);
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
}
static void test_guard_background_poll(void)
{
    /* The background poll starts queued actions only where Thread changes are allowed. */
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
    httpd_req_t req = request(HTTP_PUT, "/node/commissioner/state", "\"enable\"", NULL, "application/json");
    cJSON *json = run_request(&req, 200);
    release(&req, json);
    const char *body =
        "{\"data\":[{\"type\":\"addThreadDeviceTask\",\"attributes\":{\"pskd\":\"ABC123\",\"joinerId\":\"*\"}}]}";
    req = request(HTTP_POST, "/api/actions", body, NULL, THREAD_REST_JSON_API);
    json = run_request(&req, 200);
    release(&req, json);
    thread_rest_item_t *action = s_rest_actions;
    guard_enter_mode(BR_MODE_SNIFFER, false);
    rest_poll_guarded();
    assert(!action->active && !action->joiner_registered && mock_ot_writes == 0);
    guard_enter_mode(BR_MODE_RADIO_FAULT, false);
    rest_poll_guarded();
    assert(!action->active && !action->joiner_registered && mock_ot_writes == 0);
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
    rest_poll_guarded();
    assert(action->joiner_registered);
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
}
/* Patch 2: the worker never touches the OpenThread lock (it may be deleted during a radio push) while not ready. */
static void test_worker_does_not_touch_the_lock_when_not_ready(void)
{
    guard_enter_mode(BR_MODE_BR_RUNNING, false);
    unsigned before = mock_lock_acquires;
    rest_process_once();
    assert(mock_lock_acquires == before);
    guard_enter_mode(BR_MODE_RADIO_FAULT, false);
    rest_process_once();
    assert(mock_lock_acquires == before);
}
static void test_worker_polls_under_the_lock_when_ready(void)
{
    guard_enter_mode(BR_MODE_BR_RUNNING, true);
    unsigned before = mock_lock_acquires;
    rest_process_once();
    assert(mock_lock_acquires == before + 1);
}
/* Patch 12: a request the per-interface policy refuses never reaches the dispatcher; GET/HEAD read, the rest writes. */
static void test_netcfg_gate(void)
{
    const struct {
        httpd_method_t method;
        netcfg_service_t service;
    } rows[] = {{HTTP_GET, NETCFG_SVC_REST_READ}, {HTTP_HEAD, NETCFG_SVC_REST_READ}, {HTTP_PUT, NETCFG_SVC_REST_WRITE},
                {HTTP_POST, NETCFG_SVC_REST_WRITE}, {HTTP_DELETE, NETCFG_SVC_REST_WRITE}};
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        httpd_req_t req = request(rows[i].method, "/node/state", "", NULL, NULL);
        mock_netcfg_deny = true;
        assert(esp_otbr_thread_rest_handler(&req) == ESP_FAIL);
        assert(mock_netcfg_last_service == rows[i].service && req.status == NULL);
        cJSON_Delete(req.headers);
        mock_netcfg_deny = false;
    }
}

int main(void)
{
    mock_reset();
    assert(esp_otbr_thread_rest_register(NULL) == ESP_OK && mock_registered == 28);
    test_node();
    test_route_matrix();
    test_routing();
    test_dataset();
    test_dataset_active_while_running();
    test_actions();
    test_commissioner_and_energy();
    test_guard_br_running();
    test_guard_sniffer();
    test_guard_radio_fault();
    test_guard_mode_change_while_waiting_for_the_lock();
    test_guard_background_poll();
    test_worker_does_not_touch_the_lock_when_not_ready();
    test_worker_polls_under_the_lock_when_ready();
    test_netcfg_gate();
    rest_collection_delete(&s_rest_actions, NULL, esp_openthread_get_instance());
    rest_collection_delete(&s_rest_devices, NULL, esp_openthread_get_instance());
    rest_collection_delete(&s_rest_diagnostics, NULL, esp_openthread_get_instance());
    puts("ESP-native REST routes, datasets, collections, callbacks, and HTTP transport passed");
    return 0;
}
