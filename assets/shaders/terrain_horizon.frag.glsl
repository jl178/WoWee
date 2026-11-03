#version 450

// The horizon tiles that reach nearer than the far clip less 50 yards.
#include "terrain_horizon.glsli"

void main() {
    if (vViewDepth < push.range.x || vViewDepth > push.range.y) discard;
    outColor = horizonColor();
}
