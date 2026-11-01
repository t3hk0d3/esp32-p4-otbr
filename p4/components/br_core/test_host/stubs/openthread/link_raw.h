// Host stand-in for the one OpenThread type that mode_fsm.h needs.
#pragma once

typedef struct otInstance otInstance;
typedef struct otRadioFrame otRadioFrame;
typedef int otError;
typedef void (*otLinkRawReceiveDone)(otInstance *aInstance, otRadioFrame *aFrame, otError aError);
