#include "zigbee_link.h"

void zigbee_link_init(zigbee_link_t *link, size_t queue_depth)
{
    link->depth = queue_depth;
    link->queued = 0;
    link->client_fd = -1;
    link->have_hello = false;
    link->hello_due = false;
    link->refused = false;
    link->generation = 0;
}

bool zigbee_link_host_send(zigbee_link_t *link, size_t len)
{
    (void)len;
    if (link->client_fd < 0) {
        return true; // no client: the frame is dropped
    }
    if (link->queued >= link->depth) {
        link->refused = true;
        return false;
    }
    link->queued++;
    return true;
}

void zigbee_link_sent(zigbee_link_t *link)
{
    if (link->queued > 0) {
        link->queued--;
    }
}

bool zigbee_link_take_hello(zigbee_link_t *link, const uint8_t *frame, size_t len)
{
    if (link->have_hello || frame == NULL || len == 0 || len > ZIGBEE_LINK_MAX_FRAME) {
        return false;
    }
    link->have_hello = true;
    return true;
}

int zigbee_link_connect(zigbee_link_t *link, int fd)
{
    int previous = link->client_fd == fd ? -1 : link->client_fd;

    link->client_fd = fd;
    link->hello_due = link->have_hello;
    link->generation++; // also for a reused fd: frames queued for the old socket are stale
    return previous;
}

bool zigbee_link_disconnect(zigbee_link_t *link, int fd)
{
    if (fd < 0 || link->client_fd != fd) {
        return false;
    }
    link->client_fd = -1;
    link->hello_due = false;
    link->generation++;
    return true;
}

bool zigbee_link_take_send_hello(zigbee_link_t *link)
{
    bool due = link->hello_due;

    link->hello_due = false;
    return due;
}

uint32_t zigbee_link_generation(const zigbee_link_t *link)
{
    return link->generation;
}

bool zigbee_link_frame_current(const zigbee_link_t *link, uint32_t generation)
{
    return generation == link->generation;
}

int zigbee_link_client(const zigbee_link_t *link)
{
    return link->client_fd;
}

bool zigbee_link_accept_inbound(const zigbee_link_t *link, int fd, size_t len, bool binary)
{
    return fd >= 0 && fd == link->client_fd && binary && len > 0 && len <= ZIGBEE_LINK_MAX_FRAME;
}

bool zigbee_link_must_close(size_t len)
{
    return len > ZIGBEE_LINK_MAX_FRAME;
}

bool zigbee_link_drained(zigbee_link_t *link)
{
    if (!link->refused || link->queued >= link->depth) {
        return false;
    }
    link->refused = false;
    return true;
}
