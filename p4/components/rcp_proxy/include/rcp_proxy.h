// RCP proxy: the consumer of the rcp-proxy mode (spec 2026-11-06 radio channel mux, section RCP proxy mode). A channel
// in rcp-proxy is served to one network peer: listen or connect, over TCP or UDP. One task owns every socket; it waits
// in one select() over the adapters' sockets, the channels' eventfds and its own kick eventfd. The netcfg access rules
// apply to every peer: TCP listen clients by their socket (netcfg_socket_allowed), UDP sources and the connect target
// of both adapters by address (netcfg_peer_allowed; the mesh is refused). A frame channel's radio is reset when a new
// peer session starts (TCP accept or connect; UDP connect once per socket; UDP listen when the source differs from the
// previous peer), never when a quiet peer sends again.
#pragma once

#include "esp_err.h"
#include "radio_mux.h"

#ifdef __cplusplus
extern "C" {
#endif

// lwIP sockets the proxy holds at most: two channels in rcp-proxy at once (the C6 and the one open USB device), each
// with a TCP listener, its client and one connection being refused.
#define RCP_PROXY_SOCKETS 6
// Eventfds the proxy creates (its kick eventfd); app_main adds it to the eventfd VFS registration.
#define RCP_PROXY_EVENTFD_COUNT 1

typedef struct {
    const char *state;           // "listening", "connecting", "backoff" or "connected" (static strings)
    char peer[RADIO_MUX_PEER_MAX]; // "ip:port" ("[ip]:port" for IPv6) while connected, else empty
    // Why the session's last try to open (listen, bind) or connect failed ("tcp port 6638: Address already in use",
    // "host.local does not resolve", "connect timed out"); empty once a try worked, and while the session is new.
    char last_error[RADIO_MUX_PROXY_ERROR_MAX];
} rcp_proxy_status_t;

// Registers the rcp-proxy consumer with the mux (any firmware, no channel limit, no reboot; raw bytes on USB
// channels, frames on the C6, which it HDLC-encodes itself), then starts the proxy task (MGMT_CORE). Before
// radio_mux_start, after the eventfd VFS is registered. Once: ESP_ERR_INVALID_STATE on a second call. ESP_ERR_NO_MEM
// when the task cannot be created (the consumer stays registered and refuses attach).
esp_err_t rcp_proxy_register(void);

// Drops the current peer of the channel, if it is in rcp-proxy: TCP closes the client (listen takes the next one,
// connect dials again after its back-off), UDP lets the locked source go. Does not wait. Any task.
void rcp_proxy_kick(const char *channel_id);

// The proxy state of a channel in rcp-proxy. ESP_ERR_NOT_FOUND when the channel is not attached to the proxy. Any
// task (device_api calls it from the HTTP server task).
esp_err_t rcp_proxy_get_status(const char *channel_id, rcp_proxy_status_t *out);

#ifdef __cplusplus
}
#endif
