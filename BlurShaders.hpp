#pragma once

namespace AuraBlurShaders {
inline constexpr auto vertex = R"GLSL(#version 300 es
precision highp float;
in vec2 pos;
void main() { gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0); }
)GLSL";

// A separable Gaussian. Coordinates are framebuffer pixels throughout:
// cropping the work region must never rescale the source texture.
inline constexpr auto blur = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D tex;
uniform vec2 framebufferSize, direction;
uniform vec4 sampleBounds;
uniform float sigma;
out vec4 fragColor;
void main() {
    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    // Pixel-spaced samples avoid periodic aliasing on text and fine stripes.
    // Maximum: strength 4, scale 2, last layer sigma 48, support 144px.
    int support = int(ceil(3.0 * sigma));
    for (int i = -support; i <= support; ++i) {
        float t = float(i) / max(sigma, 0.001);
        float weight = exp(-0.5 * t * t);
        vec2 p = clamp(gl_FragCoord.xy + direction * float(i),
                       sampleBounds.xy + 0.5, sampleBounds.zw - 0.5);
        sum += texture(tex, p / framebufferSize) * weight;
        weightSum += weight;
    }
    fragColor = sum / weightSum;
}
)GLSL";

inline constexpr auto composite = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D tex;
uniform vec2 framebufferSize;
uniform mat3 rawToLogical;
uniform vec4 windowBox;
uniform float radius, roundingPower, bandHeight, reach, opacity;
uniform int layer;
uniform vec4 tint;
out vec4 fragColor;
void main() {
    vec2 p = (rawToLogical * vec3(gl_FragCoord.xy, 1.0)).xy;
    vec2 local = p - windowBox.xy;
    vec2 halfSize = windowBox.zw * 0.5;
    float r = min(radius, min(halfSize.x, halfSize.y));
    vec2 q = abs(local - halfSize) - (halfSize - r);
    vec2 corner = max(q, 0.0);
    float power = max(roundingPower, 1.0);
    float d = pow(pow(corner.x, power) + pow(corner.y, power), 1.0 / power)
              + min(max(q.x, q.y), 0.0) - r;
    float clip = 1.0 - smoothstep(-0.5, 0.5, d);
    if (clip <= 0.0) discard;
    float progress = reach > 0.0 ? clamp(1.0 - (local.y - bandHeight) / reach, 0.0, 1.0)
                                 : (local.y <= bandHeight ? 1.0 : 0.0);
    float mask;
    if (layer == 0) {
        mask = smoothstep(0.0, 1.0, progress);
        fragColor = vec4(tint.rgb, 1.0) * (tint.a * mask * clip * opacity);
    } else {
        // React Bits' five overlapping masks, directed toward the top.
        float i = float(layer);
        mask = clamp((progress - (i - 1.0) * 0.2) / 0.2, 0.0, 1.0);
        if (layer <= 3) mask *= 1.0 - clamp((progress - (i + 1.0) * 0.2) / 0.2, 0.0, 1.0);
        fragColor = texture(tex, gl_FragCoord.xy / framebufferSize) * (mask * clip * opacity);
    }
}
)GLSL";
}
