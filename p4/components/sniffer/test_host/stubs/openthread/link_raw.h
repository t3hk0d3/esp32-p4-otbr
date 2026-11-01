// Host stand-in for the OpenThread types the sniffer receive callback reads.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct otInstance otInstance;
typedef int otError;
#define OT_ERROR_NONE 0
#define OT_ERROR_FCS 12

typedef struct otRadioFrame {
    uint8_t *mPsdu;
    uint16_t mLength;
    uint8_t mChannel;
    uint8_t mRadioType;
    union {
        struct {
            uint64_t mTimestamp;
            uint32_t mAckFrameCounter;
            uint8_t mAckKeyId;
            int8_t mRssi;
            uint8_t mLqi;
        } mRxInfo;
    } mInfo;
} otRadioFrame;

typedef void (*otLinkRawReceiveDone)(otInstance *aInstance, otRadioFrame *aFrame, otError aError);
