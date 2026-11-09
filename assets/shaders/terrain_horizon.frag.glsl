#version 450

// The horizon tiles that reach nearer than the far clip less 50 yards.
#include "terrain_horizon.glsli"

void main() {
#ifdef HORIZON_COLOR
    // The far field starts where the chunks stop, by distance in the plane.
    float d = farFieldDistance();
    if (d < push.range.x || d > push.range.y) discard;
#else
    if (vViewDepth < push.range.x || vViewDepth > push.range.y) discard;
#endif
    outColor = horizonColor();
}
