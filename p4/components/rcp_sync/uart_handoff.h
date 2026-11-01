#pragma once

// Who owns UART1: the C6 log receiver or the flasher. A small state machine; the caller serialises access (jumper.c
// holds a mutex). Pure, so the stale-request scenarios run on the host.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HANDOFF_RUN,        /**< the receiver owns the UART */
    HANDOFF_PAUSE_REQ,  /**< the flasher asked for it, the receiver has not seen the request */
    HANDOFF_RELEASING,  /**< the receiver saw the request and is deleting its driver */
    HANDOFF_PAUSED,     /**< the receiver gave the UART up: the flasher owns it */
    HANDOFF_RESUME_REQ, /**< the flasher is done, the receiver has to take the UART back */
} handoff_state_t;

typedef enum {
    HANDOFF_ACTION_NONE,
    HANDOFF_ACTION_RELEASE,   /**< receiver: delete the driver, then call uart_handoff_released */
    HANDOFF_ACTION_REINSTALL, /**< receiver: install the driver again (the state is RUN already) */
} handoff_action_t;

typedef struct {
    handoff_state_t state;
} uart_handoff_t;

void uart_handoff_init(uart_handoff_t *h);

/** Flasher: ask for the UART. False unless the state is RUN, PAUSE_REQ, RELEASING or PAUSED (a resume is still pending). */
bool uart_handoff_request_pause(uart_handoff_t *h);

/** Flasher: true once the receiver has deleted its driver (the UART is free). */
bool uart_handoff_is_paused(const uart_handoff_t *h);

/**
 * Flasher: give the UART back (also after a pause that timed out or was never needed). A request the receiver never
 * saw is simply cancelled, so no token is left behind for a later pause.
 */
void uart_handoff_request_resume(uart_handoff_t *h);

/** Receiver: poll once per loop and do what is returned. */
handoff_action_t uart_handoff_poll(uart_handoff_t *h);

/** Receiver: the driver is deleted. */
void uart_handoff_released(uart_handoff_t *h);

/** Receiver: false while it must not read the UART (anything but RUN). */
bool uart_handoff_receiver_owns_uart(const uart_handoff_t *h);

#ifdef __cplusplus
}
#endif
