/*
PAD.H

The DualShock on port 1 as the Xbox's gamepad state the controller (fp_controller.h) takes.
*/
#ifndef PAD_H
#define PAD_H

#include "fp_controller.h"

/* loads the IOP's pad modules and opens port 1; returns 0 if they would not load */
int pad_init(void);

/* the pad's state this frame; returns 0 (and an idle pad) if none is connected, or is not an analog pad */
int pad_read(fp_pad *pad, float seconds);

#endif
