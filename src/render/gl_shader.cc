#include "render/gl_shader.h"

#include "core/log.h"
#include "core/profile.h"
#include "render/render_debug.h"

#include <glad/gl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace dx {
    namespace {
        const char* vertexSource = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec2 aLightUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec3 aNormal;
layout(location = 5) in vec3 aFog;
uniform mat4 uViewProj;
uniform vec2 uPan;
uniform vec2 uScale;
uniform int uFlags;
uniform vec3 uEye;
uniform vec3 uForward;
uniform vec4 uClipPlanes[8];
uniform int uClipCount;
uniform float uDetailDistance;
out float gl_ClipDistance[8];
centroid out vec2 vUV;
centroid out vec2 vLightUV;
out vec4 vColor;
out vec3 vWorld;
out vec3 vNormal;
out vec3 vFog;
void main()
{
    vFog = aFog;
    vWorld = aPosition;
    vNormal = aNormal;
    vUV = aUV * uScale + uPan;
    vLightUV = aLightUV;
    vColor = aColor;
    if ((uFlags & 32) != 0)
    {
        float depth = dot(aPosition - uEye, uForward);
        float fade = roundEven((1.0 - clamp(depth, 0.0, uDetailDistance) / uDetailDistance) * 128.0) / 255.0;
        vColor = vec4(vec3(128.0 / 255.0), fade);
    }
    for (int i = 0; i < 8; i++)
    {
        gl_ClipDistance[i] = i < uClipCount ? dot(uClipPlanes[i], vec4(aPosition, 1.0)) : 1.0;
    }
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)";

        const char* fragmentSource = R"(
centroid in vec2 vUV;
centroid in vec2 vLightUV;
in vec4 vColor;
in vec3 vWorld;
in vec3 vNormal;
in vec3 vFog;
uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform int uFlags;
uniform vec4 uTint;
uniform int uLightCount;
uniform vec4 uLightPosition[8];
uniform vec4 uLightColor[8];
uniform vec4 uLightDirection[8];
uniform int uSmoothLightmap;
uniform float uOcclusion;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 occlusionMask;
vec4 lightmap()
{
    if (uSmoothLightmap == 0)
    {
        return texture(uLightmap, vLightUV);
    }
    vec2 size = vec2(textureSize(uLightmap, 0));
    vec2 texel = vLightUV * size - 0.5;
    vec2 f = fract(texel);
    texel -= f;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;
    vec2 c0 = (texel - 0.5 + w1 / s0) / size;
    vec2 c1 = (texel + 1.5 + w3 / s1) / size;
    vec4 top = texture(uLightmap, vec2(c0.x, c0.y)) * s0.x + texture(uLightmap, vec2(c1.x, c0.y)) * s1.x;
    vec4 bottom = texture(uLightmap, vec2(c0.x, c1.y)) * s0.x + texture(uLightmap, vec2(c1.x, c1.y)) * s1.x;
    return top * s0.y + bottom * s1.y;
}
vec3 dynamicLight()
{
    vec3 total = vec3(0.0);
    vec3 normal = normalize(vNormal);
    for (int i = 0; i < uLightCount; i++)
    {
        vec3 delta = uLightPosition[i].xyz - vWorld;
        float radius = uLightPosition[i].w;
        float index = floor(dot(delta, delta) * 4093.0 / (radius * radius) + 0.5);
        if (index >= 4096.0)
        {
            continue;
        }
        float v = sqrt((index + 1.0) / 4096.0);
        float falloff = 1.0 + 2.0 * v * v * v - 3.0 * v * v;
        vec3 direction = normalize(delta);
        float attenuation = falloff * abs(dot(direction, normal));
        if (uLightDirection[i].w < 1.0)
        {
            float cosine = dot(direction, uLightDirection[i].xyz);
            if (cosine <= uLightDirection[i].w)
            {
                continue;
            }
            float spot = (cosine - uLightDirection[i].w) / (1.0 - uLightDirection[i].w);
            attenuation *= spot * spot;
        }
        total += uLightColor[i].rgb * attenuation;
    }
    return total;
}
vec4 surface()
{
    vec4 color = (uFlags & 128) != 0 ? vec4(uTint.rgb, vColor.a * uTint.a) : vColor * uTint;
    vec4 texel = vec4(1.0);
    if ((uFlags & 1) != 0)
    {
        texel = texture(uTexture, vUV);
        color *= texel;
    }
#ifdef ALPHA_TEST
    if ((uFlags & 4) != 0 && color.a < 0.5)
    {
        discard;
    }
#endif
    vec3 dynamic = vec3(0.0);
    if ((uFlags & 16) != 0)
    {
        dynamic = dynamicLight();
    }
    if ((uFlags & 2) != 0)
    {
        color.rgb *= min(lightmap().rgb + dynamic, vec3(254.0 / 255.0)) * 2.0;
    }
    else if ((uFlags & 16) != 0)
    {
        color.rgb += texel.rgb * uTint.rgb * dynamic * 2.0;
    }
    return vec4(clamp(color.rgb + vFog, 0.0, 1.0), color.a);
}
// One write to each output and no early return from main. With an early return, the Switch build
// in Ryujinx went on to overwrite fragColor with the plain surface colour, which is black for the
// detail pass, so detail-textured walls went black up close. Whether the fault is in the Switch's
// Mesa 20.1 nouveau driver or in the emulator is not known.
void main()
{
    occlusionMask = vec4(uOcclusion);
    if ((uFlags & 32) != 0)
    {
        fragColor = vec4(mix(vColor.rgb, texture(uTexture, vUV).rgb, vColor.a), 1.0);
    }
    else if ((uFlags & 64) != 0)
    {
        fragColor = lightmap();
    }
    else
    {
        fragColor = surface();
    }
}
)";

        const char* gammaVertexSource = R"(#version 330 core
void main()
{
    vec2 position = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";

        const char* gammaFragmentSource = R"(#version 330 core
uniform sampler2D uFrame;
uniform float uExponent;
out vec4 fragColor;
void main()
{
    vec3 color = texelFetch(uFrame, ivec2(gl_FragCoord.xy), 0).rgb;
    fragColor = vec4(pow(color, vec3(uExponent)), 1.0);
}
)";

        const char* fxaaFragmentSource = R"(#version 330 core
uniform sampler2D uFrame;
uniform vec2 uInverseSize;
out vec4 fragColor;
void main()
{
    vec2 uv = gl_FragCoord.xy * uInverseSize;
    vec3 rgbNW = texture(uFrame, uv + vec2(-1.0, -1.0) * uInverseSize).rgb;
    vec3 rgbNE = texture(uFrame, uv + vec2(1.0, -1.0) * uInverseSize).rgb;
    vec3 rgbSW = texture(uFrame, uv + vec2(-1.0, 1.0) * uInverseSize).rgb;
    vec3 rgbSE = texture(uFrame, uv + vec2(1.0, 1.0) * uInverseSize).rgb;
    vec3 rgbM = texture(uFrame, uv).rgb;
    vec3 weights = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, weights);
    float lumaNE = dot(rgbNE, weights);
    float lumaSW = dot(rgbSW, weights);
    float lumaSE = dot(rgbSE, weights);
    float lumaM = dot(rgbM, weights);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    vec2 direction = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 / 8.0), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, vec2(-8.0), vec2(8.0)) * uInverseSize;
    vec3 rgbA = 0.5 * (texture(uFrame, uv + direction * (1.0 / 3.0 - 0.5)).rgb + texture(uFrame, uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(uFrame, uv - direction * 0.5).rgb + texture(uFrame, uv + direction * 0.5).rgb);
    float lumaB = dot(rgbB, weights);
    fragColor = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
)";

        const char* prepareFragmentSource = R"(#version 330 core
uniform highp sampler2D uDepth;
uniform highp sampler2D uMask;
uniform vec3 uLinearize;
uniform int uDivisor;
out uint packedDepth;
void main()
{
    ivec2 source = ivec2(gl_FragCoord.xy) * uDivisor;
    float depth = uLinearize.x / (uLinearize.y - texelFetch(uDepth, source, 0).r * uLinearize.z);
    uint mask = uint(texelFetch(uMask, source, 0).r * 255.0 + 0.5);
    packedDepth = (uint(min(depth * 256.0, 16777215.0)) << 8) | mask;
}
)";

        const char* occlusionFragmentSource = R"(
uniform highp usampler2D uPacked;
uniform vec4 uUnproject;
out uint packedOcclusion;
const float radius = 40.0;
const mat2 turn = mat2(-0.7373689, 0.6754903, -0.6754903, -0.7373689);
vec3 viewPosition(ivec2 pixel, uint value)
{
    float z = float(value >> 8) / 256.0;
    return vec3((vec2(pixel) * uUnproject.xy + uUnproject.zw) * z, z);
}
vec3 neighbor(ivec2 pixel)
{
    return viewPosition(pixel, texelFetch(uPacked, pixel, 0).r);
}
void main()
{
    ivec2 size = textureSize(uPacked, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint center = texelFetch(uPacked, pixel, 0).r;
    uint receiver = center & 255u;
    float reach = radius * 256.0 / (float(center >> 8) * uUnproject.y);
    if (receiver < 3u || reach < 1.0)
    {
        packedOcclusion = 255u;
        return;
    }
    vec3 position = viewPosition(pixel, center);
    vec3 dx = pixel.x + 1 < size.x ? neighbor(pixel + ivec2(1, 0)) - position : vec3(0.0);
    vec3 dy = pixel.y + 1 < size.y ? neighbor(pixel + ivec2(0, 1)) - position : vec3(0.0);
    if (pixel.x > 0)
    {
        vec3 left = position - neighbor(pixel - ivec2(1, 0));
        if (dx == vec3(0.0) || abs(left.z) < abs(dx.z))
        {
            dx = left;
        }
    }
    if (pixel.y > 0)
    {
        vec3 down = position - neighbor(pixel - ivec2(0, 1));
        if (dy == vec3(0.0) || abs(down.z) < abs(dy.z))
        {
            dy = down;
        }
    }
    vec3 normal = normalize(cross(dx, dy));
    if (dot(normal, position) > 0.0)
    {
        normal = -normal;
    }
    reach = min(reach, 0.15 * float(size.y)) / float(SAMPLES);
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec2 direction = vec2(cos(angle), sin(angle));
    float occlusion = 0.0;
    float count = 0.0;
    for (int i = 0; i < SAMPLES; i++)
    {
        ivec2 tap = pixel + ivec2(floor(direction * (reach * (float(i) + 0.5)) + 0.5));
        direction = turn * direction;
        ivec2 inside = clamp(tap, ivec2(0), size - 1);
        float valid = tap == inside ? 1.0 : 0.0;
        uint value = texelFetch(uPacked, inside, 0).r;
        vec3 offset = viewPosition(inside, value) - position;
        float distanceSquared = dot(offset, offset);
        float cosine = (dot(offset, normal) - 2.0) * inversesqrt(distanceSquared + 0.0001);
        occlusion += valid * max(cosine, 0.0) * max(1.0 - distanceSquared / (radius * radius), 0.0) * float(value & 255u);
        count += valid;
    }
    float strength = receiver > 128u ? 0.5 : 1.0;
    float visibility = 1.0 - occlusion * strength * 3.0 / (255.0 * max(count, 1.0));
    packedOcclusion = (center & 0xFFFFFF00u) | uint(clamp(visibility, 0.0, 1.0) * 255.0 + 0.5);
}
)";

        const char* blurFragmentSource = R"(
uniform highp usampler2D uOcclusion;
uniform ivec2 uDirection;
#ifdef RESOLVE
out vec4 fragColor;
#else
out uint packedOcclusion;
#endif
const float kernel[5] = float[5](1.0, 0.8824969, 0.6065307, 0.3246525, 0.1353353);
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 limit = textureSize(uOcclusion, 0) - 1;
    uint center = texelFetch(uOcclusion, pixel, 0).r;
    float depth = float(center >> 8);
    float result = 255.0;
    if (depth > 0.0)
    {
        float tolerance = 1.0 / (depth * 0.05);
        float total = 0.0;
        float weights = 0.0;
        for (int i = -4; i <= 4; i++)
        {
            uint tap = texelFetch(uOcclusion, clamp(pixel + uDirection * i, ivec2(0), limit), 0).r;
            float weight = kernel[abs(i)] * max(0.0, 1.0 - abs(float(tap >> 8) - depth) * tolerance);
            total += float(tap & 255u) * weight;
            weights += weight;
        }
        result = total / weights;
    }
#ifdef RESOLVE
    fragColor = vec4(result / 255.0);
#else
    packedOcclusion = (center & 0xFFFFFF00u) | uint(result + 0.5);
#endif
}
)";

        const char* compositeFragmentSource = R"(#version 330 core
uniform sampler2D uOcclusion;
uniform highp sampler2D uMask;
uniform vec4 uMapping;
out vec4 fragColor;
void main()
{
    float occlusion = texture(uOcclusion, gl_FragCoord.xy * uMapping.xy + uMapping.zw).r;
    float receiver = texelFetch(uMask, ivec2(gl_FragCoord.xy), 0).r;
    fragColor = vec4(vec3(receiver < 0.01 ? 1.0 : occlusion), 1.0);
}
)";

        ShaderHandle compile(GLenum type, const char* source, const char* prefix = "")
        {
            ShaderHandle shader(glCreateShader(type));
            const char* sources[2] = { prefix, source };
            glShaderSource(shader.get(), 2, sources, nullptr);
            glCompileShader(shader.get());
            GLint ok = 0;
            glGetShaderiv(shader.get(), GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[2048] = {};
                glGetShaderInfoLog(shader.get(), sizeof(log), nullptr, log);
                throw std::runtime_error(std::string("shader compile failed: ") + log);
            }
            return shader;
        }

        // A linked program of the two stages. The stages go once it is linked, the program
        // holding what it needs of them; anything made is deleted again when a stage or the link
        // fails.
        ProgramHandle link(const char* vertexText, const char* fragmentText, const char* fragmentPrefix)
        {
            ShaderHandle vertex = compile(GL_VERTEX_SHADER, vertexText);
            ShaderHandle fragment = compile(GL_FRAGMENT_SHADER, fragmentText, fragmentPrefix);
            ProgramHandle program(glCreateProgram());
            glAttachShader(program.get(), vertex.get());
            glAttachShader(program.get(), fragment.get());
            glLinkProgram(program.get());
            GLint ok = 0;
            glGetProgramiv(program.get(), GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[2048] = {};
                glGetProgramInfoLog(program.get(), sizeof(log), nullptr, log);
                throw std::runtime_error(std::string("shader link failed: ") + log);
            }
            return program;
        }

        ProgramHandle linkFullscreen(const char* source, const char* prefix = "")
        {
            return link(gammaVertexSource, source, prefix);
        }

        void checkFramebuffer(const char* name, bool& reported)
        {
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status == GL_FRAMEBUFFER_COMPLETE || reported) {
                return;
            }
            reported = true;
            dx::log::error("DevRender", "{} framebuffer incomplete: status 0x{:04x}", name, static_cast<unsigned>(status));
        }

        // Sizes a depth and stencil texture and a red mask texture, both sampled nearest, for a
        // single-sampled copy of the scene's depth and occlusion receivers. The 2D texture bound
        // before is bound again after.
        void allocateDepthMask(GLuint depthTexture, GLuint maskTexture, int width, int height)
        {
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            glBindTexture(GL_TEXTURE_2D, depthTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, maskTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
        }

        bool matchesSceneDepth(GLuint framebuffer)
        {
            GLenum depth = framebuffer ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
            GLenum stencil = framebuffer ? GL_STENCIL_ATTACHMENT : GL_STENCIL;
            GLint type = GL_NONE;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depth, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type == GL_NONE) {
                return false;
            }
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, stencil, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type == GL_NONE) {
                return false;
            }
            GLint depthBits = 0;
            GLint stencilBits = 0;
            GLint componentType = GL_NONE;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depth, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depth, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, stencil, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
            return depthBits == 24 && stencilBits == 8 && componentType == GL_UNSIGNED_NORMALIZED;
        }
    }

    UberShader::UberShader()
    {
        for (int32_t index = 0; index < 2; index++) {
            programs[index] = link(vertexSource, fragmentSource, index ? "#version 330 core\n#define ALPHA_TEST\n" : "#version 330 core\n");
            GLuint program = programs[index].get();
            Locations& location = locations[index];
            location.viewProj = glGetUniformLocation(program, "uViewProj");
            location.flags = glGetUniformLocation(program, "uFlags");
            location.pan = glGetUniformLocation(program, "uPan");
            location.scale = glGetUniformLocation(program, "uScale");
            location.tint = glGetUniformLocation(program, "uTint");
            location.lightCount = glGetUniformLocation(program, "uLightCount");
            location.lightPosition = glGetUniformLocation(program, "uLightPosition");
            location.lightColor = glGetUniformLocation(program, "uLightColor");
            location.lightDirection = glGetUniformLocation(program, "uLightDirection");
            location.eye = glGetUniformLocation(program, "uEye");
            location.forward = glGetUniformLocation(program, "uForward");
            location.clipPlanes = glGetUniformLocation(program, "uClipPlanes");
            location.clipCount = glGetUniformLocation(program, "uClipCount");
            location.occlusion = glGetUniformLocation(program, "uOcclusion");
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
            glUniform1i(glGetUniformLocation(program, "uLightmap"), 1);
            glUniform1f(glGetUniformLocation(program, "uDetailDistance"), 380.0f);
            current = index;
            sendState();
        }
        glUseProgram(programs[current].get());

        streamVao = gl::createVertexArray();
        streamVbo = gl::createBuffer();
        glBindVertexArray(streamVao.get());
        glBindBuffer(GL_ARRAY_BUFFER, streamVbo.get());
        setupAttributes();
    }

    void UberShader::setupAttributes()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, lu)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, r)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, nx)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), reinterpret_cast<const void*>(offsetof(GLVertex, fogR)));
    }

    void UberShader::use()
    {
        glUseProgram(programs[current].get());
    }

    void UberShader::select(int32_t index)
    {
        if (index == current) {
            return;
        }
        current = index;
        glUseProgram(programs[current].get());
        if (stale[current]) {
            sendState();
            stale[current] = false;
        }
    }

    void UberShader::sendState()
    {
        const Locations& location = locations[current];
        glUniformMatrix4fv(location.viewProj, 1, GL_FALSE, state.viewProj.data());
        glUniform1i(location.flags, state.flags);
        glUniform2fv(location.pan, 1, state.pan);
        glUniform2fv(location.scale, 1, state.scale);
        glUniform4fv(location.tint, 1, state.tint);
        glUniform1i(location.lightCount, state.lightCount);
        glUniform4fv(location.lightPosition, static_cast<GLsizei>(MaxDynamicLights), state.lightPositions);
        glUniform4fv(location.lightColor, static_cast<GLsizei>(MaxDynamicLights), state.lightColors);
        glUniform4fv(location.lightDirection, static_cast<GLsizei>(MaxDynamicLights), state.lightDirections);
        glUniform3fv(location.eye, 1, state.eye);
        glUniform3fv(location.forward, 1, state.forward);
        glUniform4fv(location.clipPlanes, 8, state.clipPlanes);
        glUniform1i(location.clipCount, state.clipCount);
        glUniform1f(location.occlusion, state.occlusion);
    }

    void UberShader::setViewProj(const Mat4& matrix)
    {
        state.viewProj = matrix;
        stale[1 - current] = true;
        glUniformMatrix4fv(locations[current].viewProj, 1, GL_FALSE, matrix.data());
    }

    void UberShader::setQuality(bool smoothLightmap, float detailDistance)
    {
        for (const ProgramHandle& handle : programs) {
            GLuint program = handle.get();
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "uSmoothLightmap"), smoothLightmap ? 1 : 0);
            glUniform1f(glGetUniformLocation(program, "uDetailDistance"), detailDistance);
        }
        glUseProgram(programs[current].get());
    }

    void UberShader::setFlags(int32_t flags)
    {
        state.flags = flags;
        select((flags & SHADER_AlphaTest) ? 1 : 0);
        stale[1 - current] = true;
        glUniform1i(locations[current].flags, flags);
    }

    void UberShader::setPan(float u, float v)
    {
        state.pan[0] = u;
        state.pan[1] = v;
        stale[1 - current] = true;
        glUniform2f(locations[current].pan, u, v);
    }

    void UberShader::setScale(float u, float v)
    {
        state.scale[0] = u;
        state.scale[1] = v;
        stale[1 - current] = true;
        glUniform2f(locations[current].scale, u, v);
    }

    void UberShader::setEye(const Vector& eye, const Vector& forward)
    {
        state.eye[0] = eye.x;
        state.eye[1] = eye.y;
        state.eye[2] = eye.z;
        state.forward[0] = forward.x;
        state.forward[1] = forward.y;
        state.forward[2] = forward.z;
        stale[1 - current] = true;
        glUniform3f(locations[current].eye, eye.x, eye.y, eye.z);
        glUniform3f(locations[current].forward, forward.x, forward.y, forward.z);
    }

    void UberShader::setClipPlanes(const float* planes, int32_t count)
    {
        std::copy(planes, planes + count * 4, state.clipPlanes);
        state.clipCount = count;
        stale[1 - current] = true;
        if (count > 0) {
            glUniform4fv(locations[current].clipPlanes, count, planes);
        }
        glUniform1i(locations[current].clipCount, count);
    }

    void UberShader::setOcclusion(float weight)
    {
        state.occlusion = weight;
        stale[1 - current] = true;
        glUniform1f(locations[current].occlusion, weight);
    }

    void UberShader::setTint(float r, float g, float b, float a)
    {
        state.tint[0] = r;
        state.tint[1] = g;
        state.tint[2] = b;
        state.tint[3] = a;
        stale[1 - current] = true;
        glUniform4f(locations[current].tint, r, g, b, a);
    }

    void UberShader::setBlend(BlendMode mode)
    {
        switch (mode) {
        case BlendMode::Opaque:
        case BlendMode::Masked:
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            break;
        case BlendMode::Translucent:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
            glDepthMask(GL_FALSE);
            break;
        case BlendMode::Modulated:
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            glDepthMask(GL_FALSE);
            break;
        case BlendMode::Alpha:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            break;
        case BlendMode::Highlight:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            break;
        }
    }

    void UberShader::setDynamicLights(const std::vector<DynamicLight>& lights)
    {
        float* positions = state.lightPositions;
        float* colors = state.lightColors;
        float* directions = state.lightDirections;
        std::fill(std::begin(state.lightPositions), std::end(state.lightPositions), 0.0f);
        std::fill(std::begin(state.lightColors), std::end(state.lightColors), 0.0f);
        std::fill(std::begin(state.lightDirections), std::end(state.lightDirections), 0.0f);
        size_t count = std::min(lights.size(), MaxDynamicLights);
        for (size_t i = 0; i < count; i++) {
            const DynamicLight& light = lights[i];
            positions[i * 4 + 0] = light.location.x;
            positions[i * 4 + 1] = light.location.y;
            positions[i * 4 + 2] = light.location.z;
            positions[i * 4 + 3] = light.radius;
            colors[i * 4 + 0] = light.color.x;
            colors[i * 4 + 1] = light.color.y;
            colors[i * 4 + 2] = light.color.z;
            directions[i * 4 + 0] = light.direction.x;
            directions[i * 4 + 1] = light.direction.y;
            directions[i * 4 + 2] = light.direction.z;
            directions[i * 4 + 3] = light.cone;
        }
        state.lightCount = static_cast<int32_t>(count);
        stale[1 - current] = true;
        const Locations& location = locations[current];
        glUniform1i(location.lightCount, state.lightCount);
        glUniform4fv(location.lightPosition, static_cast<GLsizei>(MaxDynamicLights), positions);
        glUniform4fv(location.lightColor, static_cast<GLsizei>(MaxDynamicLights), colors);
        glUniform4fv(location.lightDirection, static_cast<GLsizei>(MaxDynamicLights), directions);
    }

    void UberShader::bindTexture(uint32_t texture)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
    }

    void UberShader::bindLightmap(uint32_t texture)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(GL_TEXTURE0);
    }

    int32_t UberShader::stream(const std::vector<GLVertex>& vertices)
    {
        DX_TIME("GL upload");
        size_t bytes = vertices.size() * sizeof(GLVertex);
        glBindVertexArray(streamVao.get());
        glBindBuffer(GL_ARRAY_BUFFER, streamVbo.get());
        if (bytes > streamCapacity || streamOffset + bytes > streamCapacity) {
            streamCapacity = std::max(streamCapacity, std::max(bytes, size_t { 4 } << 20));
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(streamCapacity), nullptr, GL_STREAM_DRAW);
            streamOffset = 0;
        }
        void* target = glMapBufferRange(GL_ARRAY_BUFFER, static_cast<GLintptr>(streamOffset), static_cast<GLsizeiptr>(bytes), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        std::memcpy(target, vertices.data(), bytes);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        int32_t first = static_cast<int32_t>(streamOffset / sizeof(GLVertex));
        streamOffset += bytes;
        return first;
    }

    void UberShader::draw(const std::vector<GLVertex>& vertices, uint32_t primitive)
    {
        if (vertices.empty()) {
            return;
        }
        int32_t first = stream(vertices);
        DX_TIME("GL draw");
        glDrawArrays(primitive, first, static_cast<GLsizei>(vertices.size()));
        countDraw(primitive == GL_TRIANGLES ? static_cast<int64_t>(vertices.size()) : 0);
    }

    void UberShader::upload(const std::vector<GLVertex>& vertices)
    {
        uploadBase = stream(vertices);
    }

    void UberShader::drawRange(int32_t first, int32_t count)
    {
        glBindVertexArray(streamVao.get());
        glDrawArrays(GL_TRIANGLES, uploadBase + first, count);
        countDraw(count);
    }

    StaticGeometry UberShader::createStatic(const std::vector<GLVertex>& vertices)
    {
        StaticGeometry geometry { gl::createVertexArray(), gl::createBuffer() };
        glBindVertexArray(geometry.vao.get());
        glBindBuffer(GL_ARRAY_BUFFER, geometry.buffer.get());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GLVertex)), vertices.data(), GL_STATIC_DRAW);
        setupAttributes();
        return geometry;
    }

    void UberShader::bindStatic(uint32_t vao)
    {
        glBindVertexArray(vao);
    }

    void UberShader::drawStatic(uint32_t vao, int32_t first, int32_t count)
    {
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, first, count);
        countDraw(count);
    }

    void UberShader::drawStaticRanges(uint32_t vao, const int32_t* firsts, const int32_t* counts, int32_t ranges)
    {
        glBindVertexArray(vao);
        int64_t vertices = 0;
        for (int32_t i = 0; i < ranges; i++) {
            vertices += counts[i];
        }
        countDraw(vertices);
        if (ranges == 1) {
            glDrawArrays(GL_TRIANGLES, firsts[0], counts[0]);
            return;
        }
        glMultiDrawArrays(GL_TRIANGLES, firsts, counts, ranges);
    }

    GammaPass::GammaPass()
    {
        program = linkFullscreen(gammaFragmentSource);
        exponentLocation = glGetUniformLocation(program.get(), "uExponent");
        glUseProgram(program.get());
        glUniform1i(glGetUniformLocation(program.get(), "uFrame"), 0);
        vao = gl::createVertexArray();
        texture = gl::createTexture();
        glBindTexture(GL_TEXTURE_2D, texture.get());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    void GammaPass::resize(int width, int height)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture.get());
        if (width == textureWidth && height == textureHeight) {
            return;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        textureWidth = width;
        textureHeight = height;
        if (framebuffer) {
            glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer.get());
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        }
    }

    void GammaPass::draw(int width, int height, float exponent)
    {
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glUseProgram(program.get());
        glUniform1f(exponentLocation, exponent);
        glBindVertexArray(vao.get());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        countDraw(3);
    }

    void GammaPass::apply(int width, int height, float exponent)
    {
        keep(width, height);
        draw(width, height, exponent);
    }

    void GammaPass::keep(int width, int height)
    {
        resize(width, height);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        kept = true;
    }

    bool GammaPass::read(std::vector<uint8_t>& rgba, int& width, int& height)
    {
        if (!kept || textureWidth <= 0 || textureHeight <= 0) {
            return false;
        }
        if (!readFramebuffer) {
            readFramebuffer = gl::createFramebuffer();
        }
        GLint previous = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer.get());
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.get(), 0);
        width = textureWidth;
        height = textureHeight;
        size_t stride = static_cast<size_t>(width) * 4;
        std::vector<uint8_t> raw(stride * static_cast<size_t>(height));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
        rgba.resize(raw.size());
        for (size_t y = 0; y < static_cast<size_t>(height); y++) {
            std::copy_n(raw.data() + y * stride, stride, rgba.data() + (static_cast<size_t>(height) - 1 - y) * stride);
        }
        return true;
    }

    void GammaPass::bindTarget(int width, int height)
    {
        bool allocating = !framebuffer || width != textureWidth || height != textureHeight;
        if (!framebuffer) {
            framebuffer = gl::createFramebuffer();
            depthBuffer = gl::createRenderbuffer();
            textureWidth = 0;
            textureHeight = 0;
        }
        resize(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.get());
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.get(), 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer.get());
        if (allocating) {
            static bool reported = false;
            checkFramebuffer("gamma", reported);
        }
    }

    void GammaPass::resolve(int width, int height, float exponent)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture.get());
        draw(width, height, exponent);
        kept = true;
    }

    ScenePass::ScenePass()
    {
        program = linkFullscreen(fxaaFragmentSource);
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        inverseSizeLocation = glGetUniformLocation(program.get(), "uInverseSize");
        glUseProgram(program.get());
        glUniform1i(glGetUniformLocation(program.get(), "uFrame"), 0);
        vao = gl::createVertexArray();
        texture = gl::createTexture();
        glBindTexture(GL_TEXTURE_2D, texture.get());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void ScenePass::begin(int width, int height, int samples, bool occlusion)
    {
        samples = std::min(samples, static_cast<int>(maxSamples));
        offscreen = samples > 1 || occlusion;
        occlusionActive = occlusion;
        if (!offscreen) {
            release();
            return;
        }
        if (samples < 2) {
            samples = 0;
        }
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
        if (!framebuffer) {
            framebuffer = gl::createFramebuffer();
            colorBuffer = gl::createRenderbuffer();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.get());
        bool allocating = width != bufferWidth || height != bufferHeight || samples != bufferSamples || occlusion != bufferOcclusion;
        if (allocating) {
            glBindRenderbuffer(GL_RENDERBUFFER, colorBuffer.get());
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGB8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorBuffer.get());
            if (occlusion && samples == 0) {
                if (!depthTexture) {
                    depthTexture = gl::createTexture();
                    maskTexture = gl::createTexture();
                }
                allocateDepthMask(depthTexture.get(), maskTexture.get(), width, height);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTexture.get(), 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, maskTexture.get(), 0);
                depthBuffer.reset();
                maskBuffer.reset();
            } else {
                if (!depthBuffer) {
                    depthBuffer = gl::createRenderbuffer();
                }
                glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer.get());
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer.get());
                if (occlusion) {
                    if (!maskBuffer) {
                        maskBuffer = gl::createRenderbuffer();
                    }
                    glBindRenderbuffer(GL_RENDERBUFFER, maskBuffer.get());
                    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_R8, width, height);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, maskBuffer.get());
                } else {
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, 0);
                    GLenum buffer = GL_COLOR_ATTACHMENT0;
                    glDrawBuffers(1, &buffer);
                    maskBuffer.reset();
                }
                depthTexture.reset();
                maskTexture.reset();
            }
            bufferWidth = width;
            bufferHeight = height;
            bufferSamples = samples;
            bufferOcclusion = occlusion;
        }
        if (occlusion) {
            maskWrites = true;
            GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, buffers);
        }
        if (allocating) {
            static bool reported = false;
            checkFramebuffer("scene", reported);
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    void ScenePass::end(int width, int height, bool fxaa)
    {
        if (offscreen) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer.get());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(target));
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (matchesSceneDepth(static_cast<GLuint>(target))) {
                glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(target));
            offscreen = false;
            occlusionActive = false;
        }
        if (!fxaa) {
            return;
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture.get());
        if (width != textureWidth || height != textureHeight) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            textureWidth = width;
            textureHeight = height;
        }
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glUseProgram(program.get());
        glUniform2f(inverseSizeLocation, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
        glBindVertexArray(vao.get());
        glDrawArrays(GL_TRIANGLES, 0, 3);
        countDraw(3);
    }

    bool ScenePass::occluding() const
    {
        return occlusionActive;
    }

    uint32_t ScenePass::sceneFramebuffer() const
    {
        return framebuffer.get();
    }

    uint32_t ScenePass::sceneDepth() const
    {
        return bufferOcclusion && bufferSamples == 0 ? depthTexture.get() : 0;
    }

    uint32_t ScenePass::sceneMask() const
    {
        return bufferOcclusion && bufferSamples == 0 ? maskTexture.get() : 0;
    }

    // Drops the offscreen target when the scene draws straight to the window again, so the next
    // offscreen frame allocates every buffer afresh.
    void ScenePass::release()
    {
        framebuffer.reset();
        colorBuffer.reset();
        depthBuffer.reset();
        maskBuffer.reset();
        depthTexture.reset();
        maskTexture.reset();
        bufferWidth = 0;
        bufferHeight = 0;
        bufferSamples = 0;
        bufferOcclusion = false;
    }

    void ScenePass::setMaskWrites(bool enabled)
    {
        if (!occlusionActive || enabled == maskWrites) {
            return;
        }
        maskWrites = enabled;
        GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, enabled ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1) : static_cast<GLenum>(GL_NONE) };
        glDrawBuffers(2, buffers);
    }

    void ScenePass::clearMask()
    {
        if (!occlusionActive) {
            return;
        }
        GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, buffers);
        const GLfloat zero[4] = {};
        glClearBufferfv(GL_COLOR, 1, zero);
        if (!maskWrites) {
            buffers[1] = GL_NONE;
            glDrawBuffers(2, buffers);
        }
    }

    OcclusionPass::OcclusionPass()
    {
        Program* programs[7] = { &prepare, &occlusion[0], &occlusion[1], &occlusion[2], &blur, &blurResolve, &composite };
        const char* sources[7] = { prepareFragmentSource, occlusionFragmentSource, occlusionFragmentSource, occlusionFragmentSource, blurFragmentSource, blurFragmentSource, compositeFragmentSource };
        const char* prefixes[7] = { "", "#version 330 core\n#define SAMPLES 8\n", "#version 330 core\n#define SAMPLES 12\n", "#version 330 core\n#define SAMPLES 16\n", "#version 330 core\n", "#version 330 core\n#define RESOLVE\n", "" };
        for (size_t i = 0; i < 7; i++) {
            Program& program = *programs[i];
            program.id = linkFullscreen(sources[i], prefixes[i]);
            GLuint id = program.id.get();
            program.linearize = glGetUniformLocation(id, "uLinearize");
            program.divisor = glGetUniformLocation(id, "uDivisor");
            program.unproject = glGetUniformLocation(id, "uUnproject");
            program.direction = glGetUniformLocation(id, "uDirection");
            program.mapping = glGetUniformLocation(id, "uMapping");
            glUseProgram(id);
            glUniform1i(glGetUniformLocation(id, "uPacked"), 0);
            glUniform1i(glGetUniformLocation(id, "uOcclusion"), 0);
            glUniform1i(glGetUniformLocation(id, "uDepth"), 1);
            glUniform1i(glGetUniformLocation(id, "uMask"), 2);
        }
        vao = gl::createVertexArray();
        for (size_t i = 0; i < 3; i++) {
            packedFramebuffers[i] = gl::createFramebuffer();
            packedTextures[i] = gl::createTexture();
        }
        resultFramebuffer = gl::createFramebuffer();
        resultTexture = gl::createTexture();
        uint32_t textures[4] = { packedTextures[0].get(), packedTextures[1].get(), packedTextures[2].get(), resultTexture.get() };
        for (uint32_t texture : textures) {
            GLint filter = texture == resultTexture.get() ? GL_LINEAR : GL_NEAREST;
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    void OcclusionPass::release()
    {
        releaseResolve();
        if (textureWidth == 0) {
            return;
        }
        textureWidth = 0;
        textureHeight = 0;
        textureDivisor = 0;
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        uint32_t textures[4] = { packedTextures[0].get(), packedTextures[1].get(), packedTextures[2].get(), resultTexture.get() };
        for (uint32_t texture : textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 0, 0, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
    }

    void OcclusionPass::releaseResolve()
    {
        if (!resolveFramebuffer) {
            return;
        }
        resolveFramebuffer.reset();
        depthTexture.reset();
        maskTexture.reset();
        resolveWidth = 0;
        resolveHeight = 0;
    }

    void OcclusionPass::resize(int width, int height, int divisor)
    {
        if (width == textureWidth && height == textureHeight && divisor == textureDivisor) {
            return;
        }
        textureWidth = width;
        textureHeight = height;
        textureDivisor = divisor;
        packedWidth = (width + divisor - 1) / divisor;
        packedHeight = (height + divisor - 1) / divisor;
        for (size_t i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, packedTextures[i].get());
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, packedWidth, packedHeight, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, packedFramebuffers[i].get());
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, packedTextures[i].get(), 0);
            static bool reported = false;
            checkFramebuffer("occlusion packed", reported);
        }
        glBindTexture(GL_TEXTURE_2D, resultTexture.get());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, packedWidth, packedHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, resultFramebuffer.get());
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resultTexture.get(), 0);
        static bool reported = false;
        checkFramebuffer("occlusion result", reported);
    }

    void OcclusionPass::resolve(uint32_t sceneFramebuffer, int width, int height)
    {
        if (!resolveFramebuffer) {
            resolveFramebuffer = gl::createFramebuffer();
            depthTexture = gl::createTexture();
            maskTexture = gl::createTexture();
        }
        if (width != resolveWidth || height != resolveHeight) {
            resolveWidth = width;
            resolveHeight = height;
            allocateDepthMask(depthTexture.get(), maskTexture.get(), width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, resolveFramebuffer.get());
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, maskTexture.get(), 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTexture.get(), 0);
            static bool reported = false;
            checkFramebuffer("occlusion resolve", reported);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFramebuffer.get());
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    void OcclusionPass::run(const Program& program, uint32_t target, int width, int height, uint32_t texture)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, width, height);
        glUseProgram(program.id.get());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        countDraw(3);
    }

    void OcclusionPass::apply(uint32_t sceneFramebuffer, uint32_t sceneDepth, uint32_t sceneMask, int width, int height, OcclusionQuality quality, float nearPlane, float farPlane, float tanHalfWidth, float tanHalfHeight)
    {
        GLint savedProgram = 0;
        GLint savedViewport[4] = {};
        GLint savedActive = 0;
        GLint savedTextures[3] = {};
        GLint savedVao = 0;
        GLint savedBlendSource = 0;
        GLint savedBlendDestination = 0;
        GLint savedDrawBuffers[2] = {};
        GLboolean savedDepthMask = GL_TRUE;
        GLboolean savedColorMask[4] = {};
        glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
        glGetIntegerv(GL_VIEWPORT, savedViewport);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
        for (GLint unit = 0; unit < 3; unit++) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTextures[unit]);
        }
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao);
        glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSource);
        glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDestination);
        glGetIntegerv(GL_DRAW_BUFFER0, &savedDrawBuffers[0]);
        glGetIntegerv(GL_DRAW_BUFFER1, &savedDrawBuffers[1]);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &savedDepthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
        GLenum capabilities[4] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST };
        GLboolean savedCapabilities[4] = {};
        for (size_t i = 0; i < 4; i++) {
            savedCapabilities[i] = glIsEnabled(capabilities[i]);
            glDisable(capabilities[i]);
        }
        GLboolean savedClips[8] = {};
        for (GLenum i = 0; i < 8; i++) {
            savedClips[i] = glIsEnabled(GL_CLIP_DISTANCE0 + i);
            glDisable(GL_CLIP_DISTANCE0 + i);
        }
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        int divisor = quality == OcclusionQuality::High ? 1 : 2;
        resize(width, height, divisor);
        bool attached = sceneDepth != 0;
        if (!attached) {
            resolve(sceneFramebuffer, width, height);
            sceneDepth = depthTexture.get();
            sceneMask = maskTexture.get();
        } else {
            releaseResolve();
        }
        glBindVertexArray(vao.get());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sceneDepth);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sceneMask);
        float scale = static_cast<float>(divisor);
        const Program& estimator = occlusion[static_cast<size_t>(quality) - 1];
        glUseProgram(prepare.id.get());
        glUniform3f(prepare.linearize, nearPlane * farPlane, farPlane, farPlane - nearPlane);
        glUniform1i(prepare.divisor, divisor);
        run(prepare, packedFramebuffers[0].get(), packedWidth, packedHeight, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(estimator.id.get());
        glUniform4f(estimator.unproject, 2.0f * scale * tanHalfWidth / static_cast<float>(width), 2.0f * scale * tanHalfHeight / static_cast<float>(height), (1.0f / static_cast<float>(width) - 1.0f) * tanHalfWidth, (1.0f / static_cast<float>(height) - 1.0f) * tanHalfHeight);
        run(estimator, packedFramebuffers[1].get(), packedWidth, packedHeight, packedTextures[0].get());
        glUseProgram(blur.id.get());
        glUniform2i(blur.direction, 1, 0);
        run(blur, packedFramebuffers[2].get(), packedWidth, packedHeight, packedTextures[1].get());
        glUseProgram(blurResolve.id.get());
        glUniform2i(blurResolve.direction, 0, 1);
        run(blurResolve, resultFramebuffer.get(), packedWidth, packedHeight, packedTextures[2].get());
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer);
        if (attached) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        }
        GLenum colorOnly = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &colorOnly);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        glUseProgram(composite.id.get());
        glUniform4f(composite.mapping, 1.0f / (scale * static_cast<float>(packedWidth)), 1.0f / (scale * static_cast<float>(packedHeight)), (0.5f - 0.5f / scale) / static_cast<float>(packedWidth), (0.5f - 0.5f / scale) / static_cast<float>(packedHeight));
        run(composite, sceneFramebuffer, width, height, resultTexture.get());
        if (attached) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneMask, 0);
        }

        GLenum restoredBuffers[2] = { static_cast<GLenum>(savedDrawBuffers[0]), static_cast<GLenum>(savedDrawBuffers[1]) };
        glDrawBuffers(2, restoredBuffers);
        glBlendFunc(static_cast<GLenum>(savedBlendSource), static_cast<GLenum>(savedBlendDestination));
        for (size_t i = 0; i < 4; i++) {
            if (savedCapabilities[i]) {
                glEnable(capabilities[i]);
            } else {
                glDisable(capabilities[i]);
            }
        }
        for (GLenum i = 0; i < 8; i++) {
            if (savedClips[i]) {
                glEnable(GL_CLIP_DISTANCE0 + i);
            }
        }
        glDepthMask(savedDepthMask);
        glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
        for (GLint unit = 0; unit < 3; unit++) {
            glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTextures[unit]));
        }
        glActiveTexture(static_cast<GLenum>(savedActive));
        glBindVertexArray(static_cast<GLuint>(savedVao));
        glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
        glUseProgram(static_cast<GLuint>(savedProgram));
    }
}
