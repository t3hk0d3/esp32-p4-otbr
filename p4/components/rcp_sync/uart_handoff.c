#include "uart_handoff.h"

void uart_handoff_init(uart_handoff_t *h)
{
    h->state = HANDOFF_RUN;
}

bool uart_handoff_request_pause(uart_handoff_t *h)
{
    if (h->state == HANDOFF_RESUME_REQ) {
        return false;
    }
    if (h->state == HANDOFF_RUN) {
        h->state = HANDOFF_PAUSE_REQ;
    }
    return true;
}

bool uart_handoff_is_paused(const uart_handoff_t *h)
{
    return h->state == HANDOFF_PAUSED;
}

void uart_handoff_request_resume(uart_handoff_t *h)
{
    switch (h->state) {
    case HANDOFF_PAUSE_REQ:
        h->state = HANDOFF_RUN; // never seen: nothing to give back
        break;
    case HANDOFF_RELEASING:
    case HANDOFF_PAUSED:
        h->state = HANDOFF_RESUME_REQ;
        break;
    default:
        break;
    }
}

handoff_action_t uart_handoff_poll(uart_handoff_t *h)
{
    if (h->state == HANDOFF_PAUSE_REQ) {
        h->state = HANDOFF_RELEASING;
        return HANDOFF_ACTION_RELEASE;
    }
    if (h->state == HANDOFF_RESUME_REQ) { // the driver is deleted by now: the receiver is sequential
        h->state = HANDOFF_RUN;
        return HANDOFF_ACTION_REINSTALL;
    }
    return HANDOFF_ACTION_NONE;
}

void uart_handoff_released(uart_handoff_t *h)
{
    if (h->state == HANDOFF_RELEASING) {
        h->state = HANDOFF_PAUSED;
    }
}

bool uart_handoff_receiver_owns_uart(const uart_handoff_t *h)
{
    return h->state == HANDOFF_RUN;
}
