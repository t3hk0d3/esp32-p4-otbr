#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The zigbee task's KICK ("the mailbox has work"), as plain state: at most one KICK is ever in the task's queue, the
 * queue the mux posts into without waiting, so KICKs never take the room the mux's frames, RESET and GONE need.
 *
 * `pending` is true exactly while a KICK is in the queue (or about to be posted). Any task:
 *   if (zigbee_kick_claim(k)) { post one KICK; if the post failed: zigbee_kick_unclaim(k); }
 * The zigbee task, when it takes a KICK out of the queue: zigbee_kick_taken(k), then reads the mailbox. Work posted
 * after that read finds `pending` false and posts a new KICK; work posted before it is in what the task reads.
 */
typedef struct {
    atomic_bool pending;
} zigbee_kick_t;

void zigbee_kick_init(zigbee_kick_t *k);

/** true: the caller posts one KICK now. false: one is already queued (or being posted). */
bool zigbee_kick_claim(zigbee_kick_t *k);

/** The claimed post did not get into the queue (full): the next claim posts again. */
void zigbee_kick_unclaim(zigbee_kick_t *k);

/** A KICK was taken out of the queue. Call before reading the mailbox. */
void zigbee_kick_taken(zigbee_kick_t *k);

/**
 * The backstop for a GONE the mux could not post (its queue full). Attachments are numbered from 1 by attach;
 * `detached` is the number of the attachment the mux last detached, `attached` the one the zigbee task last handled
 * the ATTACH of (0: none yet), `gone` the one it last handled a GONE for. true: the current attachment was detached
 * but its GONE never came, so the zigbee task handles it as GONE now.
 */
bool zigbee_gone_lost(uint32_t detached, uint32_t attached, uint32_t gone);

#ifdef __cplusplus
}
#endif
