/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>                  // For singly linked list

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // For mutexes and semaphores

#include "usb/usb_host.h"               // For USB device handle and transfers
#include "usb/cdc_acm_host_interface.h" // For CDC interface function table
#include "usb/usb_types_cdc.h"          // For protocol and serial state

// CDC-ACM check macros
#define CDC_ACM_CHECK(cond, ret_val) ({                             \
    if (!(cond)) {                                                  \
        return (ret_val);                                           \
    }                                                               \
})

#define CDC_ACM_CHECK_FROM_CRIT(cond, ret_val) ({                   \
    if (!(cond)) {                                                  \
        CDC_ACM_EXIT_CRITICAL();                                    \
        return ret_val;                                             \
    }                                                               \
})

// [patch 29] Bulk IN transfers kept in flight per device: while one completes into the user's data callback and is
// resubmitted, the next is already queued on the pipe, so a USB serial bridge always has a transfer to complete into.
// With 1 the driver behaves as upstream (including appending unprocessed data into the same buffer). 8: two left the
// CH340 overrunning under sustained load (the callbacks run on a priority-7 task that lwIP preempts); 8 one-packet
// transfers are ~5.5 ms at 460800 baud (hardware 2026-11-07, PATCHES.md patch 29).
#define CDC_ACM_IN_XFERS 8

typedef struct cdc_dev_s cdc_dev_t;
struct cdc_dev_s {
    cdc_acm_intf_t intf_func;             // CDC interface function table
    usb_device_handle_t dev_hdl;          // USB device handle
    void *cb_arg;                         // Common argument for user's callbacks (data IN and Notification)
    struct {
        usb_transfer_t *out_xfer;         // OUT data transfer
        usb_transfer_t *in_xfer[CDC_ACM_IN_XFERS]; // IN data transfers, submitted in index order [patch 29]
        cdc_acm_data_callback_t in_cb;    // User's callback for async (non-blocking) data IN
        uint16_t in_mps;                  // IN endpoint Maximum Packet Size
        uint8_t *in_data_buffer_base[CDC_ACM_IN_XFERS]; // Pointer to each IN transfer's data buffer [patch 29]
        size_t in_xfer_len;               // IN transfer length asked for (the buffer may be larger: cache alignment) [patch 28]
        const usb_intf_desc_t *intf_desc; // Pointer to data interface descriptor
        SemaphoreHandle_t out_mux;        // OUT mutex
        bool in_polling[CDC_ACM_IN_XFERS]; // Each BULK IN poll transfer is submitted [patch 29]
    } data;

    struct {
        usb_transfer_t *xfer;             // IN notification transfer
        const usb_intf_desc_t *intf_desc; // Pointer to notification interface descriptor, can be NULL if there is no notification channel in the device
        cdc_acm_host_dev_callback_t cb;   // User's callback for device events
        bool xfer_polling;                // INTR IN poll transfer is submitted
    } notif;                              // Structure with Notif pipe data

    usb_transfer_t *ctrl_transfer;        // CTRL (endpoint 0) transfer
    SemaphoreHandle_t ctrl_mux;           // CTRL mutex
    cdc_acm_uart_state_t serial_state;    // Serial State
    cdc_comm_protocol_t comm_protocol;
    cdc_data_protocol_t data_protocol;
    int cdc_func_desc_cnt;                // Number of CDC Functional descriptors in following array
    const usb_standard_desc_t *(*cdc_func_desc)[]; // Pointer to array of pointers to const usb_standard_desc_t
#ifdef CDC_HOST_REMOTE_WAKE_SUPPORTED
    bool remote_wakeup_enabled;           // Remote wakeup currently enabled/disabled on the device
#endif // CDC_HOST_REMOTE_WAKE_SUPPORTED
    SLIST_ENTRY(cdc_dev_s) list_entry;
};
