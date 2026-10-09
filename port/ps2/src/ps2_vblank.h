/*
PS2_VBLANK.H

The PS2's vertical blank, for the Direct3D layer (ps2_d3d_null.c): its
callback, waiting for it, and counting presented frames. ps2_vblank.c uses
ps2sdk's kernel.h, whose names (CreateThread and others) the XDK's headers
also declare, so the two never meet in one file.
*/

#ifndef __PS2_VBLANK_H
#define __PS2_VBLANK_H

typedef void (*ps2_vblank_callback)(unsigned long context);

/* the function each vertical blank calls (NULL for none) */
void ps2_vblank_set_callback(ps2_vblank_callback callback);
/* returns at the next vertical blank */
void ps2_vblank_wait(void);
/* a frame was presented: it counts as flipped at the next vertical blank */
void ps2_vblank_present(void);
/* the number of frames flipped (d3d_find_flipcount) */
volatile unsigned int *ps2_vblank_flip_count(void);

#endif /* __PS2_VBLANK_H */
