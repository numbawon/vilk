#include "preset_glsl_adapter.hpp"
#include <sstream>

namespace vilk {

// ---------------------------------------------------------------------------
// Fixed UBO block inserted after #version 450.
// Covers ALL uniforms declared in PresetShaderHeaderGlsl330.inc so the
// #define aliases work regardless of which subset the preset uses.
// ---------------------------------------------------------------------------
static const char* kPresetUBOBlock = R"GLSL(
layout(set=0, binding=0) uniform sampler2D sampler_main;
layout(set=0, binding=2) uniform sampler2D sampler_noise_lq;

layout(set=0, binding=1, std140) uniform PresetBlock {
    vec4 rand_frame;
    vec4 rand_preset;
    vec4 _c0, _c1, _c2, _c3, _c4, _c5, _c6, _c7;
    vec4 _c8, _c9, _c10, _c11, _c12, _c13;
    vec4 _qa, _qb, _qc, _qd, _qe, _qf, _qg, _qh;
    mat3x4 rot_s1,    rot_s2,    rot_s3,    rot_s4;
    mat3x4 rot_d1,    rot_d2,    rot_d3,    rot_d4;
    mat3x4 rot_f1,    rot_f2,    rot_f3,    rot_f4;
    mat3x4 rot_vf1,   rot_vf2,   rot_vf3,   rot_vf4;
    mat3x4 rot_uf1,   rot_uf2,   rot_uf3,   rot_uf4;
    mat3x4 rot_rand1, rot_rand2, rot_rand3, rot_rand4;
} ubo;

// Redirect all uniform names to the UBO instance.
#define rand_frame  ubo.rand_frame
#define rand_preset ubo.rand_preset
#define _c0  ubo._c0
#define _c1  ubo._c1
#define _c2  ubo._c2
#define _c3  ubo._c3
#define _c4  ubo._c4
#define _c5  ubo._c5
#define _c6  ubo._c6
#define _c7  ubo._c7
#define _c8  ubo._c8
#define _c9  ubo._c9
#define _c10 ubo._c10
#define _c11 ubo._c11
#define _c12 ubo._c12
#define _c13 ubo._c13
#define _qa  ubo._qa
#define _qb  ubo._qb
#define _qc  ubo._qc
#define _qd  ubo._qd
#define _qe  ubo._qe
#define _qf  ubo._qf
#define _qg  ubo._qg
#define _qh  ubo._qh
#define rot_s1    ubo.rot_s1
#define rot_s2    ubo.rot_s2
#define rot_s3    ubo.rot_s3
#define rot_s4    ubo.rot_s4
#define rot_d1    ubo.rot_d1
#define rot_d2    ubo.rot_d2
#define rot_d3    ubo.rot_d3
#define rot_d4    ubo.rot_d4
#define rot_f1    ubo.rot_f1
#define rot_f2    ubo.rot_f2
#define rot_f3    ubo.rot_f3
#define rot_f4    ubo.rot_f4
#define rot_vf1   ubo.rot_vf1
#define rot_vf2   ubo.rot_vf2
#define rot_vf3   ubo.rot_vf3
#define rot_vf4   ubo.rot_vf4
#define rot_uf1   ubo.rot_uf1
#define rot_uf2   ubo.rot_uf2
#define rot_uf3   ubo.rot_uf3
#define rot_uf4   ubo.rot_uf4
#define rot_rand1 ubo.rot_rand1
#define rot_rand2 ubo.rot_rand2
#define rot_rand3 ubo.rot_rand3
#define rot_rand4 ubo.rot_rand4

// All noise/blur variants alias to sampler_noise_lq (real binding 2).
// fw_ variants are the "previous frame" noise — same texture for now.
#define sampler_noise_mq          sampler_noise_lq
#define sampler_noise_hq          sampler_noise_lq
#define sampler_noise_lq_lite     sampler_noise_lq
#define sampler_noisevol_lq       sampler_noise_lq
#define sampler_noisevol_hq       sampler_noise_lq
#define sampler_fw_noise_lq       sampler_noise_lq
#define sampler_fw_noise_mq       sampler_noise_lq
#define sampler_fw_noise_hq       sampler_noise_lq
#define sampler_fw_noise_lq_lite  sampler_noise_lq
#define sampler_fw_noisevol_lq    sampler_noise_lq
#define sampler_fw_noisevol_hq    sampler_noise_lq
#define sampler_blur1             sampler_noise_lq
#define sampler_blur2             sampler_noise_lq
#define sampler_blur3             sampler_noise_lq

// texsize_* uniforms (w, h, 1/w, 1/h) for each stubbed sampler.
// Noise textures are typically 256×256; blur/vol textures are smaller.
const vec4 texsize_noise_lq          = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_noise_mq          = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_noise_hq          = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_noise_lq_lite     = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_noisevol_lq       = vec4(32.0,  32.0,  0.03125,    0.03125   );
const vec4 texsize_noisevol_hq       = vec4(32.0,  32.0,  0.03125,    0.03125   );
const vec4 texsize_fw_noise_lq       = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_fw_noise_mq       = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_fw_noise_hq       = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_fw_noise_lq_lite  = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_fw_noisevol_lq    = vec4(32.0,  32.0,  0.03125,    0.03125   );
const vec4 texsize_fw_noisevol_hq    = vec4(32.0,  32.0,  0.03125,    0.03125   );
const vec4 texsize_blur1             = vec4(256.0, 256.0, 0.00390625, 0.00390625);
const vec4 texsize_blur2             = vec4(128.0, 128.0, 0.0078125,  0.0078125 );
const vec4 texsize_blur3             = vec4(64.0,  64.0,  0.015625,   0.015625  );
)GLSL";

// Vertex shader for the composite (fullscreen) pass.
// Composite vertex: TEXCOORD0 = vec2 (UV), TEXCOORD1 = vec2 (rad, ang)
// Matches what projectM's composite PS declares as inputs.
static const char* kCompositeVertGlsl450 = R"GLSL(#version 450
layout(location = 0) out vec4 frag_COLOR;
layout(location = 1) out vec2 frag_TEXCOORD0;
layout(location = 2) out vec2 frag_TEXCOORD1;

void main() {
    vec2 ndc;
    ndc.x = float((gl_VertexIndex & 1) << 2) - 1.0;
    ndc.y = float((gl_VertexIndex & 2) << 1) - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vec2 uv = ndc * 0.5 + 0.5;
    frag_COLOR     = vec4(1.0);
    frag_TEXCOORD0 = uv;
    vec2 centered  = uv - 0.5;
    frag_TEXCOORD1 = vec2(length(centered) * 2.0, atan(centered.y, centered.x));
}
)GLSL";

// Warp vertex: TEXCOORD0 = vec4 (uv.xyzw, with zw=0), TEXCOORD1 = vec2 (rad, ang)
// Warp PS receives vec4 for the UV slot (projectM packs extra data there).
static const char* kWarpVertGlsl450 = R"GLSL(#version 450
layout(location = 0) out vec4 frag_COLOR;
layout(location = 1) out vec4 frag_TEXCOORD0;
layout(location = 2) out vec2 frag_TEXCOORD1;

void main() {
    vec2 ndc;
    ndc.x = float((gl_VertexIndex & 1) << 2) - 1.0;
    ndc.y = float((gl_VertexIndex & 2) << 1) - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vec2 uv = ndc * 0.5 + 0.5;
    frag_COLOR     = vec4(1.0);
    frag_TEXCOORD0 = vec4(uv, 0.0, 0.0);
    vec2 centered  = uv - 0.5;
    frag_TEXCOORD1 = vec2(length(centered) * 2.0, atan(centered.y, centered.x));
}
)GLSL";

// ---------------------------------------------------------------------------
// Helper: trim leading whitespace
// ---------------------------------------------------------------------------
static std::string_view ltrim(std::string_view s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.remove_prefix(1);
    return s;
}

// True if the line (after trim) starts with `prefix`.
static bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.substr(0, prefix.size()) == prefix;
}

// True if the type token is a sampler type.
static bool is_sampler_type(std::string_view type) {
    return starts_with(type, "sampler") || starts_with(type, "isampler") ||
           starts_with(type, "usampler");
}

// ---------------------------------------------------------------------------
// adapt_fragment — transform GLSL 330 fragment source to GLSL 450 for Vulkan.
// ---------------------------------------------------------------------------
static std::string adapt_fragment(std::string_view src) {
    std::ostringstream out;
    bool version_done = false;

    std::string_view rest = src;
    while (!rest.empty()) {
        // Extract next line
        auto nl = rest.find('\n');
        std::string_view raw_line = (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
        if (nl != std::string_view::npos) rest.remove_prefix(nl + 1);
        else                              rest = {};

        std::string_view line = ltrim(raw_line);

        // ── Version ──────────────────────────────────────────────────────────
        if (starts_with(line, "#version")) {
            out << "#version 450\n";
            out << kPresetUBOBlock << "\n";
            version_done = true;
            continue;
        }

        // ── Uniform declarations — strip all (provided via UBO / #define) ───
        if (starts_with(line, "uniform ")) {
            // Skip sampler_main too — already declared in kPresetUBOBlock.
            continue;
        }

        // ── Input attributes — add layout(location=N) ─────────────────────
        if (starts_with(line, "in ")) {
            // Determine location by semantic name suffix.
            int loc = -1;
            if (line.find("frag_COLOR")    != std::string_view::npos) loc = 0;
            if (line.find("frag_TEXCOORD0")!= std::string_view::npos) loc = 1;
            if (line.find("frag_TEXCOORD1")!= std::string_view::npos) loc = 2;

            if (loc >= 0)
                out << "layout(location=" << loc << ") " << raw_line << "\n";
            else
                out << raw_line << "\n";
            continue;
        }

        // ── Output array — add layout(location=0) ─────────────────────────
        if (starts_with(line, "out ") && line.find("rast_FragData") != std::string_view::npos) {
            out << "layout(location=0) " << raw_line << "\n";
            continue;
        }

        // ── Everything else passes through unchanged ───────────────────────
        out << raw_line << "\n";
    }

    if (!version_done) {
        // Shouldn't happen, but guard.
        out << kPresetUBOBlock;
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
GlslAdapterResult adapt_preset_glsl(std::string_view frag_glsl_330,
                                    std::string_view /*vert_glsl_330*/,
                                    bool is_warp) {
    GlslAdapterResult r;
    r.frag_glsl_450 = adapt_fragment(frag_glsl_330);
    r.vert_glsl_450 = is_warp ? kWarpVertGlsl450 : kCompositeVertGlsl450;
    r.ok = true;
    return r;
}

} // namespace vilk
