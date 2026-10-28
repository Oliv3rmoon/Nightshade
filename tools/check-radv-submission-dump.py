#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The submission-dump patch: its stamp, its build wiring, its generated code, and what it prints.

Checked without a console or a full Mesa build:
  1. The digest stamp round-trips and a changed patch invalidates a cached driver.
  2. prepare-build.sh and build-radv-dependencies.sh carry its check and its digest, in order, and
     prepare-vulkan-port.py has Eden print each pipeline's hashes beside its VkPipeline handle.
  3. The C the patch inserts into the winsys compiles on its own, against a minimal shim of the
     winsys types and the PM4 and register macros it uses (the values of src/amd/common/sid.h and
     the GFX10.3 register database), the console's memory read included.
  4. The stall dump over synthetic command streams: the waits with what memory holds now, the first
     unmet one, the draws and dispatches before it with their program addresses, and the pipelines
     that code belongs to (recorded by the code the patch puts into radv_pipeline.c), line by line.
  5. Eden's EDEN_PIPELINE line (headless/pipeline_identity.h) prints a handle as the dump does.
  6. With the pinned Mesa checkout (RADV_MESA_SOURCE, or ../mihawk-mesa-review when it exists): the
     shim's values against sid.h and src/amd/registers/gfx103.json, and the patch applied to a copy
     of the files it touches, after patch-radv-transfer-split.py, twice: the second run changes
     nothing.
Steps 3 to 5 need a C and a C++ compiler on PATH (cc, c++); without one they are skipped with a note.
"""
from pathlib import Path
import ast
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
patch = root / 'tools/patch-radv-submission-dump.py'

# 1. Stamp semantics.
with tempfile.TemporaryDirectory() as folder:
    stamp = Path(folder) / 'stamp'

    def valid(tool=patch):
        return subprocess.run([sys.executable, str(tool), '--check', str(stamp)]).returncode == 0

    assert not valid()
    stamp.write_bytes(subprocess.check_output([sys.executable, str(patch), '--digest']))
    assert valid()
    changed = Path(folder) / 'patch.py'
    changed.write_text(patch.read_text() + '\n# changed patch implementation\n')
    assert not valid(changed)
    stamp.write_text('old digest\n')
    assert not valid()

# 2. Build wiring.
prepare = (root / 'tools/prepare-build.sh').read_text()
build = (root / 'tools/build-radv-dependencies.sh').read_text()
assert 'patch-radv-submission-dump.py --check "$radv_release/EDEN_SUBMISSION_DUMP_SHA256"' in prepare
assert 'patch-radv-submission-dump.py" --digest > "$vulkan/.deps/native/radv-release/EDEN_SUBMISSION_DUMP_SHA256"' in build
# Built (the last build-radv invocation), then stamped, like every other adaptation.
assert build.index('EDEN_SUBMISSION_DUMP_SHA256') > build.rindex('build-radv.sh')
# Applied after the transfer-split patch it builds on.
assert build.index('patch-radv-submission-dump.py') > build.index('patch-radv-transfer-split.py')
port = (root / 'tools/prepare-vulkan-port.py').read_text()
assert "'::Eden::PipelineIdentity::Graphics(*pipeline, key.Hash(), key.unique_hashes);'" in port
assert "'::Eden::PipelineIdentity::Compute(*pipeline, shader_hash);'" in port
assert '#include "pipeline_identity.h"' in port
assert (root / 'headless/pipeline_identity.h').is_file()

# The inserted C, read out of the patch's string literals (and sums of them) without running its
# file edits.
tree = ast.parse(patch.read_text())
literals = {}


def evaluate(value):
    if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Add):
        return evaluate(value.left) + evaluate(value.right)
    if isinstance(value, ast.Name):
        return literals[value.id]
    return ast.literal_eval(value)


for node in tree.body:
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        try:
            literals[node.targets[0].id] = evaluate(node.value)
        except (ValueError, TypeError, KeyError):
            pass
header_fields_new = literals['header_fields_new']
header_decl_new = literals['header_decl_new']
public_decl_new = literals['public_decl_new']
stall_read_block = literals['stall_read_block']
dump_block = literals['dump_block']
pipeline_note_block = literals['pipeline_note_block']

# The PM4 encoding and the GFX10.3 registers the dump decodes (src/amd/common/sid.h, the generated
# amdgfxregs.h); step 6 compares them with the pinned Mesa when it is here.
PKT3 = {
    'PKT3_NOP': 0x10, 'PKT3_DISPATCH_DIRECT': 0x15, 'PKT3_DISPATCH_INDIRECT': 0x16, 'PKT3_COND_EXEC': 0x22,
    'PKT3_DRAW_INDIRECT': 0x24, 'PKT3_DRAW_INDEX_INDIRECT': 0x25, 'PKT3_DRAW_INDEX_2': 0x27,
    'PKT3_DRAW_INDIRECT_MULTI': 0x2C, 'PKT3_DRAW_INDEX_AUTO': 0x2D, 'PKT3_DRAW_INDEX_MULTI_AUTO': 0x30,
    'PKT3_DRAW_INDEX_OFFSET_2': 0x35, 'PKT3_WRITE_DATA': 0x37, 'PKT3_DRAW_INDEX_INDIRECT_MULTI': 0x38,
    'PKT3_WAIT_REG_MEM': 0x3C, 'PKT3_INDIRECT_BUFFER': 0x3F, 'PKT3_PFP_SYNC_ME': 0x42, 'PKT3_COND_WRITE': 0x45,
    'PKT3_EVENT_WRITE': 0x46, 'PKT3_RELEASE_MEM': 0x49, 'PKT3_DMA_DATA': 0x50, 'PKT3_ACQUIRE_MEM': 0x58,
    'PKT3_SET_CONTEXT_REG': 0x69, 'PKT3_SET_SH_REG': 0x76, 'PKT3_WAIT_ON_CE_COUNTER': 0x86,
    'PKT3_SET_SH_REG_INDEX': 0x9B, 'PKT3_NUM_INSTANCES': 0x2F,
}
OFFSETS = {'SI_SH_REG_OFFSET': 0x0000B000, 'SI_CONTEXT_REG_OFFSET': 0x00028000}
REGS = {
    'R_00B020_SPI_SHADER_PGM_LO_PS': 0xB020, 'R_00B024_SPI_SHADER_PGM_HI_PS': 0xB024,
    'R_00B120_SPI_SHADER_PGM_LO_VS': 0xB120, 'R_00B124_SPI_SHADER_PGM_HI_VS': 0xB124,
    'R_00B320_SPI_SHADER_PGM_LO_ES': 0xB320, 'R_00B324_SPI_SHADER_PGM_HI_ES': 0xB324,
    'R_00B520_SPI_SHADER_PGM_LO_LS': 0xB520, 'R_00B524_SPI_SHADER_PGM_HI_LS': 0xB524,
    'R_00B830_COMPUTE_PGM_LO': 0xB830, 'R_00B834_COMPUTE_PGM_HI': 0xB834,
    'R_028B54_VGT_SHADER_STAGES_EN': 0x28B54,
}
STAGES_EN = {'HS_EN': (2, 1), 'ES_EN': (3, 2), 'GS_EN': (5, 1), 'PRIMGEN_EN': (13, 1)}  # (shift, bits)

cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
cxx = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
notes = []

prologue = '\n'.join([
    '#include <stdint.h>', '#include <stdio.h>', '#include <string.h>', '#include <stdbool.h>',
    '#include <inttypes.h>',
    '#define PKT_TYPE_G(x)       (((x) >> 30) & 0x3)',
    '#define PKT_COUNT_G(x)      (((x) >> 16) & 0x3FFF)',
    '#define PKT3_IT_OPCODE_G(x) (((x) >> 8) & 0xFF)',
] + [f'#define {name} 0x{value:02X}' for name, value in PKT3.items()]
  + [f'#define {name} 0x{value:08X}' for name, value in OFFSETS.items()]
  + [f'#define {name} 0x{value:06X}' for name, value in REGS.items()]
  + [f'#define G_028B54_{name}(x) (((x) >> {shift}) & 0x{(1 << bits) - 1:X})'
     for name, (shift, bits) in STAGES_EN.items()]
  + [r'''
#define p_atomic_read(v) (*(v))
#define p_atomic_inc_return(v) (++*(v))
#define p_atomic_cmpxchg(v, old, _new) (*(v) == (old) ? (*(v) = (_new), (old)) : *(v))

/* A minimal stand-in for the winsys types the dump touches. */
struct radv_ps5_memory { uint8_t *cpu; uint64_t bytes; int64_t physical; };
#define RADV_PS5_MAX_IN_FLIGHT 4096
#define RADV_PS5_RING_WORDS    ((uint32_t)((16u << 20) / 4))
#define RADV_PS5_GPU_ADDRESS_LIMIT (UINT64_C(1) << 47)
'''])

struct_in_flight = 'struct radv_ps5_in_flight {\n   uint64_t seq;\n' + header_fields_new + '\n'
struct_queue = r'''
struct radv_ps5_queue {
   struct radv_ps5_memory ring;
   struct radv_ps5_in_flight in_flight[RADV_PS5_MAX_IN_FLIGHT];
   uint32_t in_flight_first;
   uint32_t in_flight_count;
   uint64_t submitted_seq;
};
'''
# The RADV types the pipeline recorder reads, as radv_pipeline.h and radv_shader.h have them.
pipeline_shim = r'''
#define RADV_PS5 1
typedef struct VkPipeline_T *VkPipeline;
enum radv_pipeline_type { RADV_PIPELINE_GRAPHICS, RADV_PIPELINE_GRAPHICS_LIB, RADV_PIPELINE_COMPUTE,
                          RADV_PIPELINE_RAY_TRACING };
enum { MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL, MESA_SHADER_GEOMETRY,
       MESA_SHADER_FRAGMENT, MESA_SHADER_COMPUTE, MESA_VULKAN_SHADER_STAGES = 14 };
#define RADV_GS_COMPUTE_SHADERS 6
struct radv_shader { uint64_t va; uint32_t code_size; };
struct radv_gs_compute_pipeline { struct radv_shader *shaders[RADV_GS_COMPUTE_SHADERS]; };
struct radv_pipeline {
   enum radv_pipeline_type type;
   bool is_internal;
   struct radv_shader *shaders[MESA_VULKAN_SHADER_STAGES];
   struct radv_shader *gs_copy_shader;
   struct radv_gs_compute_pipeline *gs_compute;
   uint64_t pipeline_hash;
};
static inline VkPipeline radv_pipeline_to_handle(struct radv_pipeline *pipeline) { return (VkPipeline)(uintptr_t)pipeline; }
static const char *_mesa_shader_stage_to_abbrev(unsigned stage)
{
   static const char *const names[] = {"VS", "TCS", "TES", "GS", "FS", "CS"};
   return stage < 6 ? names[stage] : "?";
}
'''
warnings = ['-std=c11', '-Wall', '-Wextra', '-Wshadow', '-Wimplicit-fallthrough', '-Wmissing-prototypes',
            '-Werror', '-Wno-unused-function', '-Wno-missing-field-initializers']


def compile_c(folder, name, source, link=False):
    c = Path(folder) / f'{name}.c'
    c.write_text(source)
    out = Path(folder) / name
    command = [cc, *warnings, '-o', str(out), str(c)] if link else [cc, *warnings, '-c', '-o', str(out) + '.o', str(c)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f'{name}: the inserted code did not compile')
    return out


# A synthetic command stream, packet by packet, with where each labelled packet starts.
def pkt3_header(op, body_dwords):
    return (3 << 30) | ((body_dwords - 1) << 16) | (op << 8)


class Stream:
    def __init__(self):
        self.words, self.at, self.packets = [], {}, 0

    def pkt(self, op, body, label=None):
        if label:
            self.at[label] = len(self.words)
        self.words += [pkt3_header(PKT3[op], len(body))] + body
        self.packets += 1

    def nop_pad(self):
        self.words.append(pkt3_header(PKT3['PKT3_NOP'], 0x3fff + 1))
        self.packets += 1

    def filler(self):
        self.words.append(2 << 30)

    def set_sh(self, reg, *values, index=False):
        self.pkt('PKT3_SET_SH_REG_INDEX' if index else 'PKT3_SET_SH_REG', [(reg - 0xB000) // 4] + list(values))

    def set_context(self, reg, *values):
        self.pkt('PKT3_SET_CONTEXT_REG', [(reg - 0x28000) // 4] + list(values))

    def program(self, lo, address):
        # As RADV sets a stage: PGM_LO, PGM_HI, RSRC1, RSRC2 in one packet.
        self.set_sh(REGS[lo], address >> 8, address >> 40, 0x002c0041, 0x00000090)

    def write_data(self, address, *values):
        self.pkt('PKT3_WRITE_DATA', [(5 << 8) | (1 << 20), address & 0xffffffff, address >> 32] + list(values))

    def release_mem(self, address, value, label=None):
        # RADV's cache flush and the winsys's completion: CACHE_FLUSH_AND_INV_TS_EVENT, the 32-bit value.
        self.pkt('PKT3_RELEASE_MEM', [(0x30c << 12) | (5 << 8) | 20, (1 << 29) | (3 << 24),
                                      address & 0xffffffff, address >> 32, value, 0, 0], label)

    def wait_mem(self, address, ref, function=3, mask=0xffffffff, label=None):
        self.pkt('PKT3_WAIT_REG_MEM', [(1 << 4) | function, address & 0xffffffff, address >> 32, ref, mask, 4], label)

    def wait_reg(self, reg_dw, ref, function=3, mask=0xffffffff, label=None):
        self.pkt('PKT3_WAIT_REG_MEM', [function, reg_dw, 0, ref, mask, 4], label)


FENCE, COND, UNREADABLE, MARKER = 0x200010000, 0x200010100, 0x7000000000, 0x200020000
A_VS, A_FS, B_CS, PROLOG, D_VS, D_FS, D2_VS, E_CS, LOOSE_CS, STALE_HS = (
    0x200100000, 0x200100400, 0x200200000, 0x200300000, 0x200400000, 0x200400800, 0x200410000, 0x200500000,
    0x200600000, 0x200700000)
F2, GS_MAIN = 0x200030000, 0x200800000
RELEASE_MEM_HEADER = pkt3_header(PKT3['PKT3_RELEASE_MEM'], 7)

# The shape of a Final Fantasy Resonance hang: cache-flush waits on a counter the submission's
# RELEASE_MEMs count up, and the GPU's work stopped before the third flush's write landed.
s1 = Stream()
s1.set_context(REGS['R_028B54_VGT_SHADER_STAGES_EN'], (1 << 13) | (2 << 3))  # NGG: PRIMGEN_EN, ES_EN real
s1.write_data(FENCE, 0, 0)  # radv_emit_clear_data at the command buffer's start
s1.program('R_00B520_SPI_SHADER_PGM_LO_LS', STALE_HS)  # left by an earlier pipeline; HS_EN is off
s1.program('R_00B320_SPI_SHADER_PGM_LO_ES', A_VS)
s1.program('R_00B020_SPI_SHADER_PGM_LO_PS', A_FS)
s1.pkt('PKT3_DRAW_INDEX_2', [0x1000, 0x10000, 0x2, 36, 0], 'draw1')
s1.pkt('PKT3_EVENT_WRITE', [0x407])
s1.release_mem(FENCE, 1)
s1.wait_mem(FENCE, 1, label='wait1')
s1.set_sh(REGS['R_00B830_COMPUTE_PGM_LO'], B_CS >> 8)
s1.set_sh(REGS['R_00B834_COMPUTE_PGM_HI'], B_CS >> 40)
s1.pkt('PKT3_DISPATCH_DIRECT', [8, 8, 1, 1], 'dispatch1')
s1.release_mem(FENCE, 2)
s1.wait_mem(FENCE, 2, label='wait2')
s1.filler()
s1.pkt('PKT3_COND_EXEC', [COND & 0xffffffff, COND >> 32, 0, 3], 'cond')
s1.set_context(0x28A00, 0)
s1.wait_mem(UNREADABLE, 1, function=5, label='wait3')
s1.set_sh(REGS['R_00B320_SPI_SHADER_PGM_LO_ES'], PROLOG >> 8)  # a vertex-input prolog, as RADV sets one
s1.program('R_00B020_SPI_SHADER_PGM_LO_PS', D_FS)
s1.pkt('PKT3_NUM_INSTANCES', [7])
s1.pkt('PKT3_DRAW_INDEX_AUTO', [3, 2], 'draw2')
s1.nop_pad()
s1.pkt('PKT3_DRAW_INDIRECT', [0, 4, 5, 2], 'draw3')
s1.set_sh(REGS['R_00B830_COMPUTE_PGM_LO'], LOOSE_CS >> 8, index=True)
s1.pkt('PKT3_DISPATCH_INDIRECT', [0x40, 1], 'dispatch2')
s1.release_mem(FENCE, 3)
s1.wait_mem(FENCE, 3, label='wait4')
s1.set_sh(REGS['R_00B830_COMPUTE_PGM_LO'], E_CS >> 8)
s1.pkt('PKT3_DISPATCH_DIRECT', [1, 1, 1, 1], 'dispatch3')
s1.wait_reg(0x300FC >> 2, 1, mask=1, label='wait5')
s1.pkt('PKT3_WAIT_ON_CE_COUNTER', [1], 'wait6')
s1.pkt('PKT3_EVENT_WRITE', [0x407])
s1.release_mem(FENCE, 4)
s1.wait_mem(FENCE, 4, label='wait7')
s1.pkt('PKT3_PFP_SYNC_ME', [0])
s1.release_mem(MARKER, 5)  # the winsys's completion packet

# Every wait met: the dump lists the submission's last work. No VGT_SHADER_STAGES_EN is set, so a
# draw shows every program address that is set.
s2 = Stream()
s2.write_data(F2, 0)
s2.program('R_00B520_SPI_SHADER_PGM_LO_LS', STALE_HS)
s2.program('R_00B020_SPI_SHADER_PGM_LO_PS', A_FS)
s2.pkt('PKT3_DRAW_INDEX_OFFSET_2', [3, 0, 3, 0], 'draw1')
s2.release_mem(F2, 1)
s2.wait_mem(F2, 1, label='wait1')
s2.set_sh(REGS['R_00B830_COMPUTE_PGM_LO'], E_CS >> 8)
s2.pkt('PKT3_DISPATCH_DIRECT', [2, 2, 2, 1], 'dispatch1')
s2.set_sh(REGS['R_00B830_COMPUTE_PGM_LO'], GS_MAIN >> 8)
s2.pkt('PKT3_DISPATCH_DIRECT', [4, 1, 1, 1], 'dispatch2')
s2.release_mem(MARKER, 9)

memory = {FENCE: 2, COND: 0, F2: 1, MARKER: 4}


def c_array(name, words):
    body = ',\n'.join('   ' + ', '.join(f'0x{w:08x}u' for w in words[i:i + 8]) for i in range(0, len(words), 8))
    return f'static uint32_t {name}[] = {{\n{body}\n}};\n'


test_memory = 'struct fake_word { uint64_t address; uint32_t value; };\nstatic const struct fake_word fake_memory[] = {\n' + \
    ''.join(f'   {{0x{a:x}ull, 0x{v:x}u}},\n' for a, v in memory.items()) + '};\n' + r'''
/* Stands in for the console's read (stall_read_block): the addresses above are mapped, no other. */
static bool
radv_ps5_stall_read(uint64_t address, uint32_t *value)
{
   for (size_t i = 0; i < sizeof(fake_memory) / sizeof(fake_memory[0]); i++) {
      if (fake_memory[i].address == address) {
         *value = fake_memory[i].value;
         return true;
      }
   }
   return false;
}
'''

test_main = c_array('stream1', s1.words) + c_array('stream2', s2.words) + f'''
static uint32_t ring[64];
static struct radv_shader a_vs = {{0x{A_VS:x}ull, 0x400}}, a_fs = {{0x{A_FS:x}ull, 0x200}};
static struct radv_shader b_cs = {{0x{B_CS:x}ull, 0x300}}, e_cs = {{0x{E_CS:x}ull, 0x100}};
static struct radv_shader d_vs = {{0x{D_VS:x}ull, 0x800}}, d_fs = {{0x{D_FS:x}ull, 0x300}};
static struct radv_shader d2_vs = {{0x{D2_VS:x}ull, 0x800}}, gs_main = {{0x{GS_MAIN:x}ull, 0x100}};
static struct radv_gs_compute_pipeline gs_compute = {{{{NULL, NULL, NULL, &gs_main, NULL, NULL}}}};
static struct radv_pipeline pipe_a = {{RADV_PIPELINE_GRAPHICS, false, {{&a_vs, NULL, NULL, NULL, &a_fs}}, NULL, NULL,
                                  0xa1a1a1a1a1a1a1a1ull}};
static struct radv_pipeline pipe_b = {{RADV_PIPELINE_COMPUTE, false, {{[MESA_SHADER_COMPUTE] = &b_cs}}, NULL, NULL,
                                  0xb2b2b2b2b2b2b2b2ull}};
static struct radv_pipeline pipe_d = {{RADV_PIPELINE_GRAPHICS, false, {{&d_vs, NULL, NULL, NULL, &d_fs}}, NULL,
                                  &gs_compute, 0xd4d4d4d4d4d4d4d4ull}};
static struct radv_pipeline pipe_d2 = {{RADV_PIPELINE_GRAPHICS, false, {{&d2_vs, NULL, NULL, NULL, &d_fs}}, NULL, NULL,
                                   0xd2d2d2d2d2d2d2d2ull}};
static struct radv_pipeline pipe_e = {{RADV_PIPELINE_COMPUTE, true, {{[MESA_SHADER_COMPUTE] = &e_cs}}, NULL, NULL,
                                  0xe5e5e5e5e5e5e5e5ull}};
static struct radv_pipeline pipe_rt = {{RADV_PIPELINE_RAY_TRACING, false, {{&a_vs}}, NULL, NULL, 0x1ull}};
static struct radv_queue_holder {{ struct radv_ps5_queue queue; }} holder;
/* The app's hook (headless/crash_report.cpp): the dump calls it before anything else, once. */
static unsigned stalled_calls;
void eden_gpu_stalled(void) {{ stalled_calls++; }}

int main(void)
{{
   struct radv_pipeline *const pipelines[] = {{&pipe_a, &pipe_b, &pipe_d, &pipe_d2, &pipe_e, &pipe_rt}};
   for (unsigned i = 0; i < sizeof(pipelines) / sizeof(pipelines[0]); i++)
      radv_ps5_note_pipeline_shaders(pipelines[i]);
   /* As radv_create_vs_prolog records a prolog. */
   radv_ps5_note_shader(0x{PROLOG:x}ull, 0x80, "VS prolog", RADV_PS5_SHADER_PROLOG | RADV_PS5_SHADER_INTERNAL, 0, 0);
   printf("handles a=0x%" PRIx64 " b=0x%" PRIx64 " d=0x%" PRIx64 " d2=0x%" PRIx64 "\\n",
          (uint64_t)(uintptr_t)radv_pipeline_to_handle(&pipe_a), (uint64_t)(uintptr_t)radv_pipeline_to_handle(&pipe_b),
          (uint64_t)(uintptr_t)radv_pipeline_to_handle(&pipe_d), (uint64_t)(uintptr_t)radv_pipeline_to_handle(&pipe_d2));
   fflush(stdout);

   radv_ps5_dump_stuck(stream2, sizeof(stream2) / sizeof(stream2[0]), 9);

   /* End to end: submission 4 done, 5 stuck (in a buffer of its own), 6 queued behind it. */
   struct radv_ps5_queue *const queue = &holder.queue;
   queue->ring.cpu = (uint8_t *)ring;
   ring[0] = 0x{RELEASE_MEM_HEADER:08x}u;
   queue->in_flight[0] = (struct radv_ps5_in_flight){{.seq = 4, .start = 0, .end = 8, .words = 8, .parts = 1}};
   queue->in_flight[1] = (struct radv_ps5_in_flight){{
      .seq = 5, .dedicated = {{.cpu = (uint8_t *)stream1, .bytes = sizeof(stream1), .physical = -1}},
      .words = sizeof(stream1) / sizeof(stream1[0]), .parts = 1, .signal_count = 1}};
   queue->in_flight[2] = (struct radv_ps5_in_flight){{.seq = 6, .start = 16, .end = 24, .words = 8, .parts = 1}};
   queue->in_flight_count = 3;
   queue->submitted_seq = 6;
   radv_ps5_dump_stall(queue, 5, 4);
   radv_ps5_dump_stall(queue, 5, 4); /* once per process */
   printf("stalled hook %u\\n", stalled_calls);
   return 0;
}}
'''

if not cc:
    notes.append('no C compiler on PATH: skipped the isolation compile and the stall dump run')
else:
    with tempfile.TemporaryDirectory() as folder:
        # 3. Both of the memory read's branches, and the dump, compile on their own.
        console = '\n'.join([
            '#define __PROSPERO__ 1', prologue,
            '#define PS5_KERNEL_PROT_CPU_READ 0x01',
            'int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);',
            'void radv_ps5_cpu_flush(const void *address, size_t bytes);',
            stall_read_block,
            'bool console_read(uint64_t address, uint32_t *value);',
            'bool console_read(uint64_t address, uint32_t *value) { return radv_ps5_stall_read(address, value); }',
        ])
        compile_c(folder, 'stall_read_console', console)
        compile_c(folder, 'stall_read_host', prologue + stall_read_block)
        # 4. The stall dump over the synthetic streams.
        program = compile_c(folder, 'submission_dump', prologue + struct_in_flight + struct_queue + header_decl_new + public_decl_new +
                            test_memory + dump_block + pipeline_shim + pipeline_note_block + test_main, link=True)
        run = subprocess.run([str(program)], capture_output=True, text=True, check=True)
    first, *after = run.stdout.splitlines()
    handles = dict(item.split('=') for item in first.split()[1:])
    # The app's hook ran once, before the dump: a fault in the dump then closes the app.
    assert after == ['stalled hook 1'], after
    lines = run.stderr.splitlines()
    sys.stdout.write(run.stderr)

    a1, w1 = s1.at, s2.at
    w1_words = len(s1.words)
    expected = [
        # Stream 2: no wait unmet, so its last work; no VGT_SHADER_STAGES_EN, so the stale HS shows.
        'radv/ps5 stall: seq 9: none of its 1 waits reads as unmet (0 the CPU cannot tell)',
        f'radv/ps5 stall: the last draws and dispatches before dword {len(s2.words)} (1 draws, 2 dispatches before it):',
        f'radv/ps5 stall:   draw 1 at dword {w1["draw1"]} DRAW_INDEX_OFFSET_2 count 3: HS 0x{STALE_HS:x} PS 0x{A_FS:x}',
        f'radv/ps5 stall:   dispatch 1 at dword {w1["dispatch1"]} DISPATCH_DIRECT 2x2x2 groups: CS 0x{E_CS:x}',
        f'radv/ps5 stall:   dispatch 2 at dword {w1["dispatch2"]} DISPATCH_DIRECT 4x1x1 groups: CS 0x{GS_MAIN:x}',
        f'radv/ps5 stall: HS 0x{STALE_HS:x} = no recorded shader (10 recorded, the newest 10 kept)',
        f'radv/ps5 stall: PS 0x{A_FS:x} = pipeline a1a1a1a1a1a1a1a1 vk={handles["a"]} (graphics FS)',
        f'radv/ps5 stall: CS 0x{E_CS:x} = pipeline e5e5e5e5e5e5e5e5 vk=', None,
        f'radv/ps5 stall: CS 0x{GS_MAIN:x} = pipeline d4d4d4d4d4d4d4d4 vk={handles["d"]} (graphics GS-as-compute MAIN)',
        'radv/ps5 stall: seq 9 carries 1 waits and 0 conditional runs:',
        f'radv/ps5 stall:   wait 1 at dword {w1["wait1"]}: WAIT_REG_MEM mem 0x{F2:x} & 0xffffffff == 0x00000001, '
        'now 0x00000001: met',
        # End to end: the queue's records, then the stuck one in depth.
        'radv/ps5 stall: waiting for submission 5, GPU at 4, submitted 6, 3 in flight (best-effort snapshot)',
        'radv/ps5 stall:   seq 4 done   dwords=8 agc_ibs=1 queue=0 waits=0 signals=0 depends=0 ring packets=1 draws=0'
        ' dispatches=0 gpu_waits=0 indirect=0 cond=0 completion=ok last: RELEASE_MEM',
        f'radv/ps5 stall:   seq 5 STUCK  dwords={w1_words} agc_ibs=1 queue=0 waits=0 signals=1 depends=0 dedicated'
        f' packets={s1.packets} draws=3 dispatches=3 gpu_waits=6 indirect=2 cond=1 completion=ok last:'
        ' WAIT_ON_CE_COUNTER EVENT_WRITE RELEASE_MEM WAIT_REG_MEM PFP_SYNC_ME RELEASE_MEM',
        'radv/ps5 stall:   seq 6 queued dwords=8',
        f'radv/ps5 stall: seq 5 stops at wait 4 of 7, dword {a1["wait4"]}: WAIT_REG_MEM mem 0x{FENCE:x} & 0xffffffff'
        ' == 0x00000003, now 0x00000002',
        f'radv/ps5 stall: the last draws and dispatches before dword {a1["wait4"]} (3 draws, 2 dispatches before it):',
        f'radv/ps5 stall:   draw 1 at dword {a1["draw1"]} DRAW_INDEX_2 count 36: GS 0x{A_VS:x} PS 0x{A_FS:x}',
        f'radv/ps5 stall:   dispatch 1 at dword {a1["dispatch1"]} DISPATCH_DIRECT 8x8x1 groups: CS 0x{B_CS:x}',
        f'radv/ps5 stall:   draw 2 at dword {a1["draw2"]} DRAW_INDEX_AUTO count 3 x7 instances: GS 0x{PROLOG:x} PS 0x{D_FS:x}',
        f'radv/ps5 stall:   draw 3 at dword {a1["draw3"]} DRAW_INDIRECT: GS 0x{PROLOG:x} PS 0x{D_FS:x}',
        f'radv/ps5 stall:   dispatch 2 at dword {a1["dispatch2"]} DISPATCH_INDIRECT: CS 0x{LOOSE_CS:x}',
        f'radv/ps5 stall: GS 0x{A_VS:x} = pipeline a1a1a1a1a1a1a1a1 vk={handles["a"]} (graphics VS)',
        f'radv/ps5 stall: PS 0x{A_FS:x} = pipeline a1a1a1a1a1a1a1a1 vk={handles["a"]} (graphics FS)',
        f'radv/ps5 stall: CS 0x{B_CS:x} = pipeline b2b2b2b2b2b2b2b2 vk={handles["b"]} (compute CS)',
        f'radv/ps5 stall: GS 0x{PROLOG:x} = a vertex-input prolog (RADV\'s own; the draw\'s vertex shader runs after'
        ' it, in the pipeline its PS names)',
        f'radv/ps5 stall: PS 0x{D_FS:x} = pipeline d2d2d2d2d2d2d2d2 vk={handles["d2"]} (graphics FS)',
        f'radv/ps5 stall: PS 0x{D_FS:x} = pipeline d4d4d4d4d4d4d4d4 vk={handles["d"]} (graphics FS, recorded earlier at this address)',
        f'radv/ps5 stall: CS 0x{LOOSE_CS:x} = no recorded shader (10 recorded, the newest 10 kept)',
        'radv/ps5 stall: seq 5 carries 7 waits and 1 conditional runs:',
        f'radv/ps5 stall:   wait 1 at dword {a1["wait1"]}: WAIT_REG_MEM mem 0x{FENCE:x} & 0xffffffff == 0x00000001,'
        ' now 0x00000002: passed (a later write in this submission left that value)',
        f'radv/ps5 stall:   wait 2 at dword {a1["wait2"]}: WAIT_REG_MEM mem 0x{FENCE:x} & 0xffffffff == 0x00000002,'
        ' now 0x00000002: met',
        f'radv/ps5 stall:   cond at dword {a1["cond"]}: COND_EXEC mem 0x{COND:x} (the next 3 dwords run if it is not 0),'
        ' now 0x00000000: skips',
        f'radv/ps5 stall:   wait 3 at dword {a1["wait3"]}: WAIT_REG_MEM mem 0x{UNREADABLE:x} & 0xffffffff >= 0x00000001:'
        ' cannot tell: the CPU cannot read that memory',
        f'radv/ps5 stall:   wait 4 at dword {a1["wait4"]}: WAIT_REG_MEM mem 0x{FENCE:x} & 0xffffffff == 0x00000003,'
        ' now 0x00000002: UNMET, the first: the GPU stops here',
        f'radv/ps5 stall:   wait 5 at dword {a1["wait5"]}: WAIT_REG_MEM reg 0x300fc & 0x00000001 == 0x00000001:'
        ' not reached',
        f'radv/ps5 stall:   wait 6 at dword {a1["wait6"]}: WAIT_ON_CE_COUNTER: not reached',
        f'radv/ps5 stall:   wait 7 at dword {a1["wait7"]}: WAIT_REG_MEM mem 0x{FENCE:x} & 0xffffffff == 0x00000004,'
        ' now 0x00000002: not reached',
    ]
    # Each expected line in order; a None entry means "the previous line is a prefix".
    position = 0
    for index, line in enumerate(expected):
        if line is None:
            continue
        prefix = index + 1 < len(expected) and expected[index + 1] is None or line.endswith('queued dwords=8')
        while position < len(lines) and not (lines[position].startswith(line) if prefix else lines[position] == line):
            position += 1
        assert position < len(lines), f'missing or out of order:\n  {line}'
        position += 1
    text = run.stderr
    # Nothing after the stop is listed as work before it, and a disabled stage is not shown.
    assert f'CS 0x{E_CS:x}' not in text.split('radv/ps5 stall: seq 5 stops')[1], 'work after the stop was listed'
    assert f'HS 0x{STALE_HS:x}' not in text.split('radv/ps5 stall: seq 5 stops')[1], 'a disabled stage was listed'
    # The ray tracing pipeline and the GS-as-compute shader were recorded as they should be.
    assert text.count('waiting for submission') == 1, 'the dump runs once per process'
    assert 'pipeline 0000000000000001' not in text, 'a ray tracing pipeline was recorded'
    assert f'pipeline e5e5e5e5e5e5e5e5 vk=0x' in text and ', RADV internal)' in text

    # 5. Eden's line names a handle as the driver's does (vk=0x<lowercase hex>).
    if not cxx:
        notes.append('no C++ compiler on PATH: skipped the EDEN_PIPELINE line')
    else:
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'identity.cpp'
            source.write_text(r'''
#include "pipeline_identity.h"
struct VkPipeline_T;
int main() {
    auto* const graphics = reinterpret_cast<VkPipeline_T*>(std::uintptr_t{0x80c41f000});
    ::Eden::PipelineIdentity::Graphics(graphics, 0x0123456789abcdefull,
                                       {0, 0x1111111111111111ull, 0, 0, 0, 0x6666666666666666ull});
    ::Eden::PipelineIdentity::Compute(reinterpret_cast<VkPipeline_T*>(std::uintptr_t{0x80c41f100}),
                                      0xfedcba9876543210ull);
    ::Eden::PipelineIdentity::Compute(std::uint64_t{0x1234}, 1);
}
''')
            binary = Path(folder) / 'identity'
            result = subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                                     '-I' + str(root / 'headless'), str(source), '-o', str(binary)],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                sys.stderr.write(result.stderr)
                raise SystemExit('headless/pipeline_identity.h did not compile')
            eden = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stderr.splitlines()
        sys.stdout.write('\n'.join(eden) + '\n')
        assert eden == [
            'EDEN_PIPELINE vk=0x80c41f000 graphics key=123456789abcdef va=0 vb=1111111111111111 tc=0 te=0 gs=0 '
            'fs=6666666666666666',
            'EDEN_PIPELINE vk=0x80c41f100 compute cs=fedcba9876543210',
            'EDEN_PIPELINE vk=0x1234 compute cs=1',
        ], eden
        assert re.fullmatch(r'vk=0x[0-9a-f]+', 'vk=' + handles['a']), handles

# 6. Against the pinned Mesa, when it is here.
mesa = Path(os.environ.get('RADV_MESA_SOURCE') or root.parent / 'mihawk-mesa-review')
if not (mesa / 'src/amd/common/sid.h').is_file():
    notes.append(f'no pinned Mesa at {mesa}: skipped the sid.h and register comparison and the twice-applied patch')
else:
    sid = (mesa / 'src/amd/common/sid.h').read_text()
    for name, value in {**PKT3, **OFFSETS}.items():
        found = re.search(rf'^#define\s+{name}\s+(0x[0-9A-Fa-f]+)\b', sid, re.M)
        assert found and int(found.group(1), 16) == value, f'{name}: sid.h has {found and found.group(1)}'
    gfx103 = json.loads((mesa / 'src/amd/registers/gfx103.json').read_text())
    at = {m['name']: m['map']['at'] for m in gfx103['register_mappings']}
    for name, value in REGS.items():
        register = name.split('_', 2)[2]
        assert at.get(register) == value, f'{name}: gfx103.json has {at.get(register)}'
    fields = {f['name']: f['bits'] for f in gfx103['register_types']['VGT_SHADER_STAGES_EN']['fields']}
    for name, (shift, bits) in STAGES_EN.items():
        assert fields[name] == [shift, shift + bits - 1], f'VGT_SHADER_STAGES_EN.{name}: {fields[name]}'
    touched = ['src/amd/vulkan/radv_cmd_buffer.c', 'src/amd/vulkan/meta/radv_meta_buffer.c',
               'src/amd/vulkan/meta/radv_meta_copy.c', 'src/amd/vulkan/radv_pipeline.c',
               'src/amd/vulkan/radv_shader.c', 'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.h',
               'src/amd/vulkan/winsys/ps5/radv_ps5_winsys_public.h', 'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c',
               'src/amd/vulkan/winsys/ps5/radv_ps5_cs.c']
    with tempfile.TemporaryDirectory() as folder:
        # The patches run from a copy of tools/, so the driver receipt they set aside when they change
        # something (../mihawk-vulkan-review) is not this machine's.
        tools = Path(folder) / 'repo/tools'
        tools.mkdir(parents=True)
        for name in ('patch-radv-transfer-split.py', 'patch-radv-submission-dump.py'):
            shutil.copyfile(root / 'tools' / name, tools / name)
        tree = Path(folder) / 'radv-src'
        for relative in touched:
            (tree / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(mesa / relative, tree / relative)

        def apply():
            for name in ('patch-radv-transfer-split.py', 'patch-radv-submission-dump.py'):
                out = subprocess.run([sys.executable, '-B', str(tools / name), str(tree)], capture_output=True,
                                     text=True, check=True).stdout
            return {relative: (tree / relative).read_text() for relative in touched}, out

        first, out = apply()
        assert 'changed=True' in out, out
        second, out = apply()
        assert 'changed=False' in out and second == first, 'a second run changed the driver source'
        winsys = first['src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c']
        assert winsys.count('radv_ps5_dump_stuck(words, count, record->seq);') == 1
        assert first['src/amd/vulkan/radv_pipeline.c'].count('   radv_ps5_note_pipeline_shaders(pipeline);') == 1
        assert first['src/amd/vulkan/radv_shader.c'].count('radv_ps5_note_shader(prolog->va') == 1

for note in notes:
    print(f'note: {note}')
print('PASS: stamp, build wiring, the inserted winsys code compiles in isolation, and the stall dump names the '
      'first unmet wait, the work before it and the pipelines it runs' + (' (with skips)' if notes else ''))
