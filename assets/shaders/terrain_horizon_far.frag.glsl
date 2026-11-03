#version 450

// The horizon tiles wholly past the near limit: no discard (TerrainHorizon).
#include "terrain_horizon.glsli"

void main() {
    outColor = horizonColor();
}
