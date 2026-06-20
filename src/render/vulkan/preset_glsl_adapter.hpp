#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace vilk {

// GPU-side UBO matching std140 layout of the GLSL PresetBlock.
// Field names track the MilkDrop/projectM convention.
//
// Offsets (std140, column-major):
//   rand_frame  :   0 (vec4,  16 B)
//   rand_preset :  16 (vec4,  16 B)
//   c[0..13]    :  32 (14×vec4, 224 B)   _c0.._c13
//   q[0..7]     : 256 (8×vec4,  128 B)   _qa.._qh
//   rot[0..23]  : 384 (24×mat3x4, 1152 B)
//   Total       : 1536 B
struct PresetUBO {
    std::array<float,4> rand_frame;      //  0
    std::array<float,4> rand_preset;     // 16
    std::array<float,4> c[14];           // 32   c[0]=_c0 .. c[13]=_c13
    std::array<float,4> q[8];            // 256  q[0]=_qa .. q[7]=_qh
    float               rot[24][3][4];   // 384  24×mat3x4 col-major
};
static_assert(sizeof(PresetUBO) == 1536, "PresetUBO size mismatch");

// Indices into c[] for commonly-used _cN aliases:
//   c[2] = {time, fps, frame, progress}
//   c[3] = {bass, mid, treb, vol}
//   c[4] = {bassAtt, midAtt, trebAtt, volAtt}
//   c[7] = {vpW, vpH, 1/vpW, 1/vpH}
//   c[8..11] = trig-of-time rows (cos/sin combos)

struct GlslAdapterResult {
    std::string frag_glsl_450; // Vulkan-ready GLSL 450 fragment source
    std::string vert_glsl_450; // Vulkan-ready GLSL 450 vertex source
    bool        ok = false;
    std::string error;
};

// Transform projectM's GLSL 330 output to Vulkan GLSL 450.
//
// Transformations applied:
//   - #version 330  →  #version 450
//   - All non-sampler uniforms stripped (provided via PresetUBO at binding 1)
//   - sampler_main kept at layout(set=0, binding=0); all other samplers stripped
//   - Fixed PresetBlock UBO inserted with #define aliases for all _cN / _qN / rot_* names
//   - in TYPE frag_*  →  layout(location=N) in TYPE frag_*  (N per semantic)
//   - out vec4 rast_FragData[N]  →  layout(location=0) out vec4 rast_FragData[N]
//
// The caller is responsible for also producing a matching vertex shader via
// adapt_preset_vert_glsl().
// is_warp=true → uses the warp vertex shader (TEXCOORD0=vec4);
// is_warp=false → uses the composite vertex shader (TEXCOORD0=vec2).
GlslAdapterResult adapt_preset_glsl(std::string_view frag_glsl_330,
                                    std::string_view vert_glsl_330,
                                    bool is_warp = false);

} // namespace vilk
