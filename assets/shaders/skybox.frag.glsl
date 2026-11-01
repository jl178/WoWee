#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    vec4 topColor;        // LightIntBand 2, the zenith
    vec4 middleColor;     // 3, at 18 degrees
    vec4 band1Color;      // 4, at 10 degrees
    vec4 band2Color;      // 5, at 3 degrees
    vec4 smogColor;       // 6, on the horizon
    vec4 fogColorPush;    // 7, from 30 degrees below the horizon down
    vec4 sunDirAndTime;   // xyz = sun direction, w = timeOfDay
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    // The stock dome: six colours at fixed elevations, linear between them
    // in angle. Nothing else is added - the glow around the sun is its own
    // glare sprite, drawn by Celestial.
    float elevDeg = degrees(asin(clamp(worldDir.z, -1.0, 1.0)));
    vec3 sky;
    if (elevDeg >= 18.0) {
        sky = mix(push.middleColor.rgb, push.topColor.rgb, (elevDeg - 18.0) / 72.0);
    } else if (elevDeg >= 10.0) {
        sky = mix(push.band1Color.rgb, push.middleColor.rgb, (elevDeg - 10.0) / 8.0);
    } else if (elevDeg >= 3.0) {
        sky = mix(push.band2Color.rgb, push.band1Color.rgb, (elevDeg - 3.0) / 7.0);
    } else if (elevDeg >= 0.0) {
        sky = mix(push.smogColor.rgb, push.band2Color.rgb, elevDeg / 3.0);
    } else if (elevDeg >= -30.0) {
        sky = mix(push.fogColorPush.rgb, push.smogColor.rgb, (elevDeg + 30.0) / 30.0);
    } else {
        sky = push.fogColorPush.rgb;
    }

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
