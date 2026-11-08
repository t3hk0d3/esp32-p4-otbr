// Between builtin_rcp.c (chip-independent) and the board's RCP file (c6.c).
#pragma once

#include <stdbool.h>

#include "radio_mux.h"
#include "rcp_sync.h"

// The board's RCP: its chip and label, and its ops (the id comes from builtin_rcp.c).
typedef struct {
    const char *chip;
    const char *label;
    bool (*present)(radio_mux_fault_kind_t *kind, char *reason, size_t cap);
    esp_err_t (*boot_steps)(radio_mux_fault_kind_t *kind, char *reason, size_t cap);
    esp_err_t (*push)(const void *source, char *detail, size_t cap, bool *base_only);
    esp_err_t (*rom_recover)(char *detail, size_t cap);
} builtin_rcp_board_t;

// The board's RCP (c6.c).
extern const builtin_rcp_board_t builtin_rcp_board;

// The status follows a probe: the installed version, and the recovery flag after the recovery it may have run.
void builtin_rcp_note_probe(const rcp_sync_probe_result_t *probe);
// A manual ROM recovery: the flag follows it when it was an attempt.
void builtin_rcp_note_recovery(bool attempted, bool succeeded);
