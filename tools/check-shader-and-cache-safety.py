#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host ASan/UBSan checks for Vulkan integer texel buffers, shader env bounds, and cache payload validation.

Usage: check-shader-and-cache-safety.py EDEN_SOURCE
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1])

host_info_h = (source / 'src/shader_recompiler/host_translate_info.h').read_text()
shader_info_h = (source / 'src/shader_recompiler/shader_info.h').read_text()
tex_pass_cpp = (source / 'src/shader_recompiler/ir_opt/texture_pass.cpp').read_text()
spirv_ctx_h = (source / 'src/shader_recompiler/backend/spirv/spirv_emit_context.h').read_text()
spirv_ctx_cpp = (source / 'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text()
spirv_img_cpp = (source / 'src/shader_recompiler/backend/spirv/emit_spirv_image.cpp').read_text()
vk_cache_cpp = (source / 'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp').read_text()
gl_cache_cpp = (source / 'src/video_core/renderer_opengl/gl_shader_cache.cpp').read_text()
env_h = (source / 'src/video_core/shader_environment.h').read_text()
env_cpp = (source / 'src/video_core/shader_environment.cpp').read_text()
decode_cpp = (source / 'src/shader_recompiler/frontend/maxwell/decode.cpp').read_text()
prep_py = (root / 'tools/prepare-vulkan-port.py').read_text()
reader_inc = (root / 'headless/shader_cache_reader.inc').read_text()

# Patch 42 invariants
assert 'bool support_integer_texture_buffers{};' in host_info_h
assert 'bool is_integer = false;' in shader_info_h
assert 'texture_buffer_descriptors[index].is_integer |= desc.is_integer;' in tex_pass_cpp
assert 'host_info.support_integer_texture_buffers &&' in tex_pass_cpp
assert 'Id image_type;' in spirv_ctx_h and 'Id pointer_type;' in spirv_ctx_h
assert 'desc.is_integer ? U32[1] : F32[1]' in spirv_ctx_cpp
assert 'ctx.texture_buffers.at(info.descriptor_index).is_integer' in spirv_img_cpp
assert 'constexpr u32 CACHE_VERSION = 19;' in vk_cache_cpp
assert '.support_integer_texture_buffers = true,' in vk_cache_cpp

# Patch 43 invariants
assert 'Texture descriptor index {} exceeds TIC limit {}' in env_cpp
assert 'Graphics constant buffer index {} exceeds count {}' in env_cpp
assert 'Compute constant buffer index {} exceeds count {}' in env_cpp

# Patch 44 invariants
assert 'Shader dump failed:' in vk_cache_cpp
assert "expected = 1  # graphics exception dump logs without rebuilding CFG (patch 44)" in prep_py

# Patch 45 invariants
assert 'if (insn == 0 || insn == ~u64{0})' in decode_cpp

# Patch 46 invariants (including prepare-vulkan-port.py PS5 Vulkan override!)
assert 'static_vector<Shader::Environment*, 5>' not in vk_cache_cpp
assert 'static_vector<Shader::Environment*, 5>' not in gl_cache_cpp
assert 'static_vector<Shader::Environment*, 5>' not in prep_py
assert 'static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;' in prep_py
assert 'if (expected_envs != envs.size())' in prep_py
assert 'if (initial_offset >= code.size_bytes())' in env_cpp
assert 'if (index >= code.size())' in env_cpp

# Patch 47 invariants
assert 'void Deserialize(std::ifstream& file, std::streampos cache_end);' in env_h
assert 'env.Deserialize(file, std::streampos{end});' in reader_inc
assert 'if (num_envs != 1)' in reader_inc

# Compile and run the REAL Deserialize validation block under ASan/UBSan.
d_start = env_cpp.index('    if (stage > Shader::Stage::VertexA')
d_end = env_cpp.index('    code.resize(code_size / sizeof(u64));', d_start)
validate_block = env_cpp[d_start:d_end]

test_cpp = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <vector>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Shader {
enum class Stage : u32 { VertexB, TessellationControl, TessellationEval, Geometry, Fragment, Compute, VertexA };
enum class TextureType : u32 {};
enum class TexturePixelFormat : u32 {};
enum class ReplaceConstant : u32 {};
}

static bool Validate(Shader::Stage stage, u64 code_size, u32 read_lowest, u32 read_highest,
                     u64 num_texture_types, u64 num_texture_pixel_formats,
                     u64 num_cbuf_values, u64 num_cbuf_replacement_values,
                     u64 remaining_bytes) {
    std::string payload(remaining_bytes, '\0');
    std::istringstream file(payload);
    const std::streampos cache_end = static_cast<std::streamoff>(remaining_bytes);
    std::array<u32, 3> workgroup_size{};
    u32 shared_memory_size{};
    std::array<char, 80> sph{};
    std::array<u32, 8> gp_passthrough_mask{};
    (void)workgroup_size; (void)shared_memory_size; (void)sph; (void)gp_passthrough_mask;
    try {
''' + validate_block + r'''
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

int main() {
    // Valid 16-byte compute entry: code(16) + workgroup(12) + shared(4) = 32 bytes.
    assert(Validate(Shader::Stage::Compute, 16, 0, 8, 0, 0, 0, 0, 32));
    // 1 byte short fails before allocation.
    assert(!Validate(Shader::Stage::Compute, 16, 0, 8, 0, 0, 0, 0, 31));
    // Mismatched read_highest (0 for 16-byte code) rejected.
    assert(!Validate(Shader::Stage::Compute, 16, 0, 0, 0, 0, 0, 0, 64));
    // Non-multiple-of-8 or zero code_size rejected.
    assert(!Validate(Shader::Stage::Compute, 0, 0, 0, 0, 0, 0, 0, 64));
    assert(!Validate(Shader::Stage::Compute, 12, 0, 4, 0, 0, 0, 0, 64));
    // Out-of-range stage rejected.
    assert(!Validate(static_cast<Shader::Stage>(7), 16, 0, 8, 0, 0, 0, 0, 128));
    // Huge texture count overflowing multiplication (would wrap without division check!) rejected.
    assert(!Validate(Shader::Stage::Compute, 16, 0, 8, (u64{1} << 62), 0, 0, 0, 128));
    // 100 texture types (800 bytes) in a 200-byte remaining file rejected before reading.
    assert(!Validate(Shader::Stage::Compute, 16, 0, 8, 100, 0, 0, 0, 200));
    // Geometry shader requires sph(80) + gp_passthrough_mask(32) + code(16) = 128 bytes.
    assert(Validate(Shader::Stage::Geometry, 16, 0, 8, 0, 0, 0, 0, 128));
    assert(!Validate(Shader::Stage::Geometry, 16, 0, 8, 0, 0, 0, 0, 127));
    std::puts("Shader & cache safety: PASS integer texel buffer, env bounds, 6-stage prepare-vulkan-port, and payload budget validation");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='eden-shader-cache-safety-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
        '-x', 'c++', '-', '-o', str(exe),
    ], input=test_cpp, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
