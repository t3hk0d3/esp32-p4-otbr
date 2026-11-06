// TCP side of the proxy: the listener, refusing a connection, the client's options and the coalesced send.
// Accepting is the one piece connect mode would replace (spec section 5).
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "usb_serial_priv.h"

#if !CONFIG_LWIP_SO_LINGER
#error "CONFIG_LWIP_SO_LINGER is off: a refused connection would not be reset; enable it in p4/sdkconfig.defaults, else p4/sdkconfig predates it: delete p4/sdkconfig and build again (docs/development.md)."
#endif

static const char *TAG = "usb_serial";

#define LISTEN_BACKLOG 1
#define KEEP_IDLE_S 10
#define KEEP_INTVL_S 2
#define KEEP_COUNT 3

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int usb_serial_tcp_listen(uint16_t port)
{
    const int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    const int zero = 0;

    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)); // dual-stack: IPv4 clients too
    struct sockaddr_in6 sa = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "listening on port %u: errno %d", port, errno);
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    ESP_LOGI(TAG, "listening on TCP port %u", port);
    return fd;
}

void usb_serial_tcp_refuse(int fd)
{
    const struct linger lg = {.l_onoff = 1, .l_linger = 0};

    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
}

static void peer_text(int fd, char *out, size_t cap)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    out[0] = '\0';
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return;
    }
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, out, cap);
    } else if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&addr;

        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) { // an IPv4 client on the dual-stack socket
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], out, cap);
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, out, cap);
        }
    }
}

void usb_serial_tcp_client_setup(int fd, char *peer, size_t cap)
{
    const int one = 1;
    const int idle = KEEP_IDLE_S;
    const int intvl = KEEP_INTVL_S;
    const int count = KEEP_COUNT;

    // Nagle plus delayed ACK would add ~200 ms stalls to ASH and spinel exchanges.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A vanished client is closed within about 16 s instead of lwIP's two hours.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    set_nonblocking(fd);
    peer_text(fd, peer, cap);
}

bool usb_serial_tcp_flush(int fd, uint64_t *sent)
{
    for (int run = 0; run < 2; run++) { // the ring's head run, then the wrapped part
        const uint8_t *p;
        const size_t n = usb_serial_rx_peek(&p);

        if (n == 0) {
            return true;
        }
        const ssize_t r = send(fd, p, n, MSG_DONTWAIT);

        if (r < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK; // full send buffer: the bytes stay in the ring
        }
        usb_serial_rx_consume((size_t)r);
        *sent += (uint64_t)r;
        if ((size_t)r < n) {
            return true;
        }
    }
    return true;
}
