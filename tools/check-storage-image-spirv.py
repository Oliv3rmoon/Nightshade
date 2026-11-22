#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate storage image declarations emitted by pinned Eden plus the complete patch set.

Run with EDEN_ARCHIVE SIRIT_SOURCE [SIRIT_LIBRARY]. Sirit is the real emitter used by Eden;
the optional library avoids rebuilding it. During make test these are found in its build cache.
The production declaration methods and their type helpers are compiled unchanged in a small
Sirit context, then spirv-val checks reads, writes, and atomics through scalar and indexed descriptors.
Removing the array declaration must fail on OpAccessChain into a non-composite image.
EDEN_SHADER_BUILD instead exercises the source named by that build's compile_commands.json,
then compiles its complete translation unit without changing the build's source or objects.
This also catches a host build that forgot to apply the production patch set.
"""
from pathlib import Path
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
archive = Path(sys.argv[1]) if len(sys.argv) > 1 else root / '.deps/eden-67bada77.tar.gz'
cache_file = root / '.local/headless-cache'
cache = Path(cache_file.read_text().strip()) if cache_file.is_file() else root / '.local'
sirit_header = next((cache / 'source/.cache/cpm/sirit').glob('*/include/sirit/sirit.h'), None)
sirit = Path(sys.argv[2]) if len(sys.argv) > 2 else (
    sirit_header.parents[2] if sirit_header else root / '.local/missing-sirit')
library = Path(sys.argv[3]) if len(sys.argv) > 3 else None
if not (sirit / 'include/sirit/sirit.h').is_file():
    sys.exit('Pass the Eden build cache\'s Sirit source directory as argument 2')
assert hashlib.sha256(archive.read_bytes()).hexdigest() == json.loads(
    (root / 'UPSTREAM.json').read_text())['archives'][archive.name], 'wrong Eden archive'


def extract(text, signature, suffix=''):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + suffix


with tempfile.TemporaryDirectory(prefix='storage-image-spirv-') as tmp:
    tmp = Path(tmp)
    tree = tmp / 'eden'
    patches = sorted((root / 'tools/eden-patches').glob('*.diff'))
    files = {line[6:] for p in patches for line in p.read_text().splitlines()
             if line.startswith('+++ b/')}
    files |= {'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp',
              'src/shader_recompiler/backend/spirv/spirv_emit_context.h',
              'src/shader_recompiler/shader_info.h', 'src/shader_recompiler/stage.h'}
    with tarfile.open(archive) as tar:
        for member in tar:
            relative = member.name.split('/', 1)[-1]
            if relative not in files or not member.isfile():
                continue
            target = tree / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
    for patch in patches:
        subprocess.run(['patch', '-d', str(tree), '-p1', '-N', '-s', '-i', str(patch)], check=True)
    entry = None
    source = tree
    if build := os.environ.get('EDEN_SHADER_BUILD'):
        commands = json.loads((Path(build) / 'compile_commands.json').read_text())
        entry, = [e for e in commands if e['file'].endswith('/spirv/spirv_emit_context.cpp')]
        source = Path(entry['file']).parents[4]
    cpp = (source / 'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text()
    header = (source / 'src/shader_recompiler/backend/spirv/spirv_emit_context.h').read_text()
    image = (source / 'src/shader_recompiler/backend/spirv/emit_spirv_image.cpp').read_text()
    atomics = (source / 'src/shader_recompiler/backend/spirv/emit_spirv_image_atomic.cpp').read_text()
    info = (source / 'src/shader_recompiler/shader_info.h').read_text()
    stage = (source / 'src/shader_recompiler/stage.h').read_text()
    types = '\n'.join([extract(stage, 'enum class Stage :', ';'),
                       extract(info, 'enum class TextureType :', ';'),
                       extract(info, 'enum class ImageFormat :', ';'),
                       extract(info, 'struct ImageDescriptor {', ';'),
                       extract(info, 'struct ImageBufferDescriptor {', ';'),
                       extract(header, 'struct ImageDefinition {', ';'),
                       extract(header, 'struct ImageBufferDefinition {', ';')])
    functions = '\n'.join(extract(cpp, signature) for signature in (
        'spv::ImageFormat GetImageFormat(',
        'Id ImageType(EmitContext& ctx, const ImageDescriptor&',
        'std::string_view StageName(', 'template <typename Desc>\nstd::string NameOf(',
        'Id DescType(', 'void EmitContext::DefineImageBuffers(', 'void EmitContext::DefineImages('))
    functions += '\n' + '\n'.join(extract(image, signature, suffix) for signature, suffix in (
        ('enum class NonUniformKind', ';'), ('[[nodiscard]] bool IsNonUniformSupported(', ''),
        ('void DecorateNonUniform(', ''), ('[[nodiscard]] bool MarkNonUniform(', ''),
        ('struct StorageImageRef', ';'), ('template <typename Definition>\nStorageImageRef StorageImage(', ''),
        ('StorageImageRef StorageImage(EmitContext& ctx, const IR::Value& index, IR::TextureInstInfo', ''),
        ('std::pair<Id, bool> Image(EmitContext& ctx, const IR::Value& index, IR::TextureInstInfo', ''),
        ('Id EmitImageAtomicTexelPointer(', '')))
    functions += '\n' + '\n'.join(extract(atomics, signature) for signature in (
        'std::pair<Id, Id> AtomicArgs(', 'Id ImageAtomicU32(', 'Id EmitImageAtomicIAdd32('))
    fixture = (root / 'tools/storage-image-spirv.cpp.in').read_text()
    (tmp / 'check.cpp').write_text(fixture.replace('// DESCRIPTOR_TYPES', types)
                                 .replace('// PRODUCTION_FUNCTIONS', functions))
    includes = [sirit / 'include', Path('/opt/homebrew/include')]
    cpm_roots = {cache / 'source/.cache/cpm', sirit.parents[1]}
    for cpm in cpm_roots:
        for name in ('fmt', 'spirv-headers'):
            includes += list((cpm / name).glob('*/include'))
    # Compiling Sirit's small source set also works when its headers come from Eden's CPM
    # cache rather than a system installation; do not require its CMake install metadata.
    emitter = [str(library)] if library else [str(p) for p in sorted((sirit / 'src').rglob('*.cpp'))]
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-DFMT_HEADER_ONLY',
                    *['-I' + str(i) for i in [*includes, sirit / 'src']], str(tmp / 'check.cpp'),
                    *emitter, '-o', str(tmp / 'check')], check=True)
    subprocess.run([str(tmp / 'check'), str(tmp)], check=True)
    modules = sorted(tmp.glob('*.spv'))
    assert len(modules) == 66, f'expected 66 SPIR-V modules, got {len(modules)}'
    failures = []
    for module in modules:
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(module)],
                                text=True, capture_output=True)
        if result.returncode:
            failures.append(f'{module.name}: {result.stdout}{result.stderr}')
    assert not failures, '\n'.join(failures)
    if entry:
        args = entry.get('arguments') or shlex.split(entry['command'])
        args[args.index('-o') + 1] = str(tmp / 'spirv_emit_context.o')
        subprocess.run(args, cwd=entry['directory'], check=True)
        print('Storage image SPIR-V: actual build source spirv_emit_context.cpp compiled with build flags')
    print(f'Storage image SPIR-V: PASS {len(modules)} modules; '
          'scalar/array images and image buffers, float/integer, reads/writes/atomics, '
          'SPIR-V 1.0/1.3/1.4, dynamic indexing, descriptor counts 1/2/8/1024')
