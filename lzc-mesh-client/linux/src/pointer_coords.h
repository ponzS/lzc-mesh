#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// The virtual-pointer protocol uses integer coordinates AND integer extents.
// Use 1/4096 pixel units, rather than truncating the desktop position to pixels.
// Rounding before normalising also removes integer-boundary round-trip errors.
typedef struct { uint32_t x, y, width, height; } PointerCoords;
static inline bool pointer_coords(double nx,double ny,int width,int height,PointerCoords *out) {
    if (width<=0 || height<=0 || width>100000 || height>100000 || !isfinite(nx) || !isfinite(ny)) return false;
    double x=fmax(0.0,fmin(nx*(double)width,(double)width-1.0));
    double y=fmax(0.0,fmin(ny*(double)height,(double)height-1.0));
    // A single subunit places exact pixel boundaries just above, rather than
    // below, the boundary after the compositor's floating-point normalisation.
    // The bias is < 1/256 pixel and disappears in Wayland's 24.8 event coords.
    out->x=(uint32_t)llround(x*4096.0)+1u;out->y=(uint32_t)llround(y*4096.0)+1u;
    out->width=(uint32_t)width*4096u;out->height=(uint32_t)height*4096u;
    return true;
}
