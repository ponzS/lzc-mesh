// Regression properties against the production coordinate mapper, independent
// of networking: preserve pixel boundaries, fractions, monotonic motion, bounds.
#include <assert.h>
#include <stdio.h>
#include "pointer_coords.h"
int main(void) {
    const int widths[]={1920,1080,2560,3840};unsigned count=0;
    for (unsigned n=0;n<sizeof(widths)/sizeof(widths[0]);n++) {
        int w=widths[n];double previous=-1;
        for (int i=0;i<w;i++) {
            PointerCoords p;assert(pointer_coords((double)i/w,.5,w,1080,&p));
            double restored=(double)p.x/p.width*w;
            assert(fabs(restored-i)<1.0/512.0);
            assert(restored>previous);previous=restored;count++;
        }
        for (int i=0;i<1000;i++) {
            double expected=(double)i/8.0;
            PointerCoords p;assert(pointer_coords(expected/w,.5,w,1080,&p));
            assert(fabs((double)p.x/p.width*w-expected)<1.0/512.0);count++;
        }
    }
    PointerCoords p;assert(pointer_coords(-1,2,1920,1080,&p));assert((int)((double)p.x/p.width*1920)==0 && (int)((double)p.y/p.height*1080)==1079);
    assert(!pointer_coords(NAN,0,1920,1080,&p));assert(!pointer_coords(0,0,0,1080,&p));
    printf("pointer mapping properties passed: %u pixel/fraction cases\n",count);
}
