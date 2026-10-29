#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""When a GPU submission stops making progress, say what it carried before the console is gone.

The PS5 winsys gives up on a submission whose completion marker has not moved for ten seconds and
prints only "the GPU has not reached submission N (at M)" (radv_ps5_winsys.c). That names the
stuck sequence number but nothing about the work: #277 (Nintendo Switch Sports) takes the whole
console down a few frames in, #236 and #233 (Luigi's Mansion 3, a demo) end in
VK_ERROR_DEVICE_LOST at a fixed scene, and none of them says what the GPU was running when it
stopped. No crash report is written when the console itself dies, so the one chance to learn
anything is this stderr line, flushed before the machine goes.

This adds a dump at that ten-second point. The winsys already keeps a record of every submission
still in flight (its sequence number, where its words sit in the ring or a buffer of its own);
this records a little more for each (its size in dwords, how many AGC submissions it was split
into, the queue it went to, the waits and signals it carried and the sequence it depends on) and,
when the wait trips, walks the stuck submission and the ones queued behind it: it decodes their
PM4, counts the packets, draws, dispatches, GPU waits (WAIT_REG_MEM), indirect and conditional
packets, checks that each ends with the completion RELEASE_MEM, and lists the last few opcodes.
A submission that ends without its completion packet, or that carries a GPU wait that never
signalled, or an indirect/conditional packet, shows up here by name. Everything is behind the
ten-second branch, so a healthy queue pays only a few field stores per submission, under the
submit lock it already holds.

Every Final Fantasy Resonance hang (Oliv3rmoon/Nightshade#1) stops in a submission of one shape:
ten thousand dwords, 348 draws, nine dispatches and 23 WAIT_REG_MEMs, most of them RADV's own
cache-flush waits on a counter the submission's RELEASE_MEMs count up (radv_cs.c). So the stuck
submission is now read in depth as well:
  - each wait it carries (WAIT_REG_MEM, WAIT_ON_CE_COUNTER) and each conditional run (COND_EXEC):
    the address, reference, mask and compare function, what the CPU reads at that address now
    when it can (sceKernelQueryMemoryProtection says whether the address is mapped for the CPU;
    the line is flushed from the CPU cache first, as the marker poll does), and the first wait
    whose condition does not hold. A wait on a counter that has since moved past it does not
    hold either; one whose current value a later write in the same submission explains has been
    passed, so the first wait that is neither is where the GPU stopped;
  - the last draws and dispatches before that wait (or before the end, when no wait is unmet),
    with the program each runs: the SPI_SHADER_PGM_LO/HI (PS, VS, ES as the NGG or legacy GS
    stage, LS as HS) and COMPUTE_PGM_LO/HI values the SET_SH_REG packets set before it, the
    stages VGT_SHADER_STAGES_EN enables (GFX10.3 offsets, src/amd/registers/gfx103.json);
  - whose code each of those addresses is: pipeline creation (radv_pipeline.c, through
    radv_pipeline_report_pso_history, which every graphics and compute pipeline passes) records
    each shader's GPU range with the pipeline's hash and its VkPipeline handle, and RADV's
    vertex-input prologs (radv_shader.c) are recorded too, in a fixed ring of 8192 entries with
    no allocation and no lock. Eden prints the same handle beside its own hashes once per
    pipeline it builds (headless/pipeline_identity.h, EDEN_PIPELINE on stderr), so stderr names
    the game's shader the GPU is stuck in.

The threaded recording layer (radv_threaded_layer.c, which #277 points at) is compiled into the
driver but only runs when RADV_THREADED_RECORDING is set, which ProsperoEden never sets, so it
records nothing in a shipping build; naming the Vulkan command types would need a change above the
winsys and is not done here.

Applied to the driver checkout after patch-radv-transfer-split.py; running it again changes
nothing. tools/check-radv-submission-dump.py compiles the inserted winsys code on its own and runs
the stuck-submission walk over synthetic command streams.
"""
from pathlib import Path
import sys
import hashlib

fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / 'mihawk-vulkan-review/.deps/work/radv-src'
vulkan = src / 'src/amd/vulkan'
ps5 = vulkan / 'winsys/ps5'
changed = False


def patch(path, edits):
    global changed
    text = path.read_text()
    new = text
    for old, replacement, marker in edits:
        if marker in new:
            continue  # already patched
        assert new.count(old) == 1, f'{path.name}: pinned code changed: {old[:60]!r}'
        new = new.replace(old, replacement)
    if new != text:
        path.write_text(new)
        changed = True


# 1. The per-submission record gains the few fields the dump prints, and the winsys header
#    declares the recorder the submit path calls.
header_fields_old = '''   uint32_t start;
   uint32_t end;
   struct radv_ps5_memory dedicated;
};'''
header_fields_new = '''   uint32_t start;
   uint32_t end;
   struct radv_ps5_memory dedicated;
   /* ProsperoEden (tools/patch-radv-submission-dump.py): what the stall dump prints; filled
    * under submit_lock once the submission is claimed, 0 until then. */
   uint32_t words;
   uint16_t parts;
   uint16_t wait_count;
   uint16_t signal_count;
   uint8_t queue_index;
   uint64_t depends;
};'''

header_decl_old = 'struct radv_ps5_queue *radv_ps5_queue_get(void);\n'
header_decl_new = ('struct radv_ps5_queue *radv_ps5_queue_get(void);\n'
                   '/* ProsperoEden: records for the newest in-flight submission what the stall dump needs\n'
                   ' * (tools/patch-radv-submission-dump.py). Called with the submit lock held. */\n'
                   'void radv_ps5_record_submission(struct radv_ps5_queue *queue, uint32_t words, uint32_t parts,\n'
                   '                                uint32_t wait_count, uint32_t signal_count, uint32_t queue_index,\n'
                   '                                uint64_t depends);\n')

patch(ps5 / 'radv_ps5_winsys.h', [
    (header_fields_old, header_fields_new, 'uint32_t words;\n   uint16_t parts;'),
    (header_decl_old, header_decl_new, 'void radv_ps5_record_submission('),
])

# 2. The public winsys header declares what RADV's pipeline code calls to say where a shader is.
public_decl_old = ('void radv_ps5_winsys_heap_usage(uint64_t *pool_bytes, uint64_t *available_bytes, '
                   'uint64_t *driver_bytes);\n')
public_decl_new = public_decl_old + '''
/* ProsperoEden (tools/patch-radv-submission-dump.py): a shader's code sits at [va, va + bytes) on
 * the GPU, for the stall dump to say whose code a stuck draw or dispatch was running: the stage, the
 * kind of code (RADV_PS5_SHADER_*), the pipeline's hash and its VkPipeline handle (0 for code no
 * pipeline owns). stage is a string that lives as long as the process. Called at pipeline and
 * vertex-input prolog creation: a few stores into a fixed ring, no allocation, no lock. */
#define RADV_PS5_SHADER_GRAPHICS 0u
#define RADV_PS5_SHADER_COMPUTE 1u
#define RADV_PS5_SHADER_PROLOG 2u
#define RADV_PS5_SHADER_INTERNAL 0x100u /* RADV's own: a meta pipeline or a prolog */
void radv_ps5_note_shader(uint64_t va, uint32_t bytes, const char *stage, uint32_t kind, uint64_t pipeline_hash,
                          uint64_t pipeline);
'''

patch(ps5 / 'radv_ps5_winsys_public.h', [
    (public_decl_old, public_decl_new, 'void radv_ps5_note_shader('),
])

# 3. The dump itself, the recorder and the shader ring, in the winsys, before the wait that detects
#    the stall; and the call from inside that wait when it trips.
winsys_include_old = '#include "sid.h"\n'
winsys_include_new = ('#include "sid.h"\n'
                      '#if defined(__PROSPERO__)\n'
                      '#include <ps5platform/kernel.h> /* ProsperoEden: tools/patch-radv-submission-dump.py */\n'
                      '#endif\n')

# What the CPU reads at a GPU address the stuck submission waits on. Kept apart from dump_block so
# the host check can stand in a memory of its own (tools/check-radv-submission-dump.py).
stall_read_block = r'''/* ProsperoEden (tools/patch-radv-submission-dump.py): the 32-bit value at a GPU address, as the
 * CPU reads it now. A buffer's GPU address is its CPU address (radv_ps5_bo.c), but the address may
 * belong to a buffer since freed, so the kernel is asked first whether it is mapped for the CPU;
 * the line is then flushed from the CPU cache, as the marker poll does, so the read sees memory.
 * Only the stall dump calls it. */
static bool
radv_ps5_stall_read(uint64_t address, uint32_t *value)
{
#if defined(__PROSPERO__)
   if (address % 4 || address >= RADV_PS5_GPU_ADDRESS_LIMIT)
      return false;
   void *start = NULL, *end = NULL;
   uint32_t protection = 0;
   if (sceKernelQueryMemoryProtection((void *)(uintptr_t)address, &start, &end, &protection) != 0 ||
       (uintptr_t)start > address || (uintptr_t)end < address + sizeof(uint32_t) ||
       !(protection & PS5_KERNEL_PROT_CPU_READ))
      return false;
   radv_ps5_cpu_flush((const void *)(uintptr_t)address, sizeof(uint32_t));
   *value = *(const volatile uint32_t *)(uintptr_t)address;
   return true;
#else
   /* The host model runs no GPU, so nothing stalls. */
   (void)address;
   (void)value;
   return false;
#endif
}

'''

dump_block = r'''/* ProsperoEden (tools/patch-radv-submission-dump.py): when a submission's words stop making
 * progress for RADV_PS5_STALL_NS, say what the stuck submission and the ones queued behind it
 * carry, before the console is gone. Nothing here runs until the ten-second wait trips, but for
 * radv_ps5_record_submission (a few stores per submission) and radv_ps5_note_shader (a few per
 * shader, at pipeline creation). */

static const char *
radv_ps5_pkt3_name(uint32_t op)
{
   switch (op) {
   case PKT3_NOP: return "NOP";
   case PKT3_DRAW_INDEX_AUTO: return "DRAW_INDEX_AUTO";
   case PKT3_DRAW_INDEX_2: return "DRAW_INDEX_2";
   case PKT3_DRAW_INDEX_OFFSET_2: return "DRAW_INDEX_OFFSET_2";
   case PKT3_DRAW_INDEX_MULTI_AUTO: return "DRAW_INDEX_MULTI_AUTO";
   case PKT3_DRAW_INDIRECT: return "DRAW_INDIRECT";
   case PKT3_DRAW_INDEX_INDIRECT: return "DRAW_INDEX_INDIRECT";
   case PKT3_DRAW_INDIRECT_MULTI: return "DRAW_INDIRECT_MULTI";
   case PKT3_DRAW_INDEX_INDIRECT_MULTI: return "DRAW_INDEX_INDIRECT_MULTI";
   case PKT3_DISPATCH_DIRECT: return "DISPATCH_DIRECT";
   case PKT3_DISPATCH_INDIRECT: return "DISPATCH_INDIRECT";
   case PKT3_WAIT_REG_MEM: return "WAIT_REG_MEM";
   case PKT3_WAIT_ON_CE_COUNTER: return "WAIT_ON_CE_COUNTER";
   case PKT3_PFP_SYNC_ME: return "PFP_SYNC_ME";
   case PKT3_COND_EXEC: return "COND_EXEC";
   case PKT3_COND_WRITE: return "COND_WRITE";
   case PKT3_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
   case PKT3_RELEASE_MEM: return "RELEASE_MEM";
   case PKT3_EVENT_WRITE: return "EVENT_WRITE";
   case PKT3_ACQUIRE_MEM: return "ACQUIRE_MEM";
   case PKT3_DMA_DATA: return "DMA_DATA";
   case PKT3_WRITE_DATA: return "WRITE_DATA";
   default: return NULL;
   }
}

static bool
radv_ps5_pkt3_is_draw(uint32_t op)
{
   switch (op) {
   case PKT3_DRAW_INDEX_AUTO:
   case PKT3_DRAW_INDEX_2:
   case PKT3_DRAW_INDEX_OFFSET_2:
   case PKT3_DRAW_INDEX_MULTI_AUTO:
   case PKT3_DRAW_INDIRECT:
   case PKT3_DRAW_INDEX_INDIRECT:
   case PKT3_DRAW_INDIRECT_MULTI:
   case PKT3_DRAW_INDEX_INDIRECT_MULTI:
      return true;
   default:
      return false;
   }
}

struct radv_ps5_stream_scan {
   uint32_t packets;
   uint32_t draws;
   uint32_t dispatches;
   uint32_t waits;
   uint32_t indirect;
   uint32_t conditional;
   uint32_t last_op[6];
   uint32_t last_count;
   bool ends_with_release_mem;
   bool truncated;
};

/* Walks the submission's PM4 words, counting packets and the kinds the stall hypotheses turn on,
 * and remembering the last few opcodes. Bounded by count; the NOP padding this winsys emits is
 * one header-only word (PKT3_NOP_PAD, count 0x3fff). */
static void
radv_ps5_scan_stream(const uint32_t *words, uint32_t count, struct radv_ps5_stream_scan *scan)
{
   memset(scan, 0, sizeof(*scan));
   uint32_t last = UINT32_MAX;
   for (uint32_t i = 0; i < count;) {
      const uint32_t header = words[i];
      const uint32_t type = PKT_TYPE_G(header);
      if (type == 2) { /* a type-2 filler word is one dword */
         i += 1;
         continue;
      }
      if (type != 3) {
         scan->truncated = true;
         break;
      }
      const uint32_t op = PKT3_IT_OPCODE_G(header);
      const uint32_t body = PKT_COUNT_G(header);
      const uint32_t n = (op == PKT3_NOP && body == 0x3fff) ? 1 : body + 2;
      scan->packets++;
      last = op;
      if (scan->last_count < 6) {
         scan->last_op[scan->last_count++] = op;
      } else {
         memmove(scan->last_op, scan->last_op + 1, 5 * sizeof(uint32_t));
         scan->last_op[5] = op;
      }
      if (radv_ps5_pkt3_is_draw(op))
         scan->draws++;
      switch (op) {
      case PKT3_DRAW_INDIRECT:
      case PKT3_DRAW_INDEX_INDIRECT:
      case PKT3_DRAW_INDIRECT_MULTI:
      case PKT3_DRAW_INDEX_INDIRECT_MULTI:
      case PKT3_INDIRECT_BUFFER: scan->indirect++; break;
      case PKT3_DISPATCH_DIRECT: scan->dispatches++; break;
      case PKT3_DISPATCH_INDIRECT: scan->dispatches++; scan->indirect++; break;
      case PKT3_WAIT_REG_MEM: scan->waits++; break;
      case PKT3_COND_EXEC:
      case PKT3_COND_WRITE: scan->conditional++; break;
      default: break;
      }
      if (i + n > count) {
         scan->truncated = true;
         break;
      }
      i += n;
   }
   scan->ends_with_release_mem = last == PKT3_RELEASE_MEM;
}

/* The words a record holds, and whether they lie inside the buffer they claim: a record caught
 * mid-claim must not send a walk out of bounds. */
static bool
radv_ps5_record_words(const struct radv_ps5_queue *queue, const struct radv_ps5_in_flight *record,
                      const uint32_t **words, uint32_t *count)
{
   const bool dedicated = record->dedicated.cpu != NULL;
   uint32_t n = record->words;
   if (!n && !dedicated)
      n = record->end - record->start;
   *words = dedicated ? (const uint32_t *)record->dedicated.cpu : (const uint32_t *)queue->ring.cpu + record->start;
   *count = n;
   if (!n || n > RADV_PS5_RING_WORDS)
      return false;
   if (dedicated)
      return (uint64_t)n * sizeof(uint32_t) <= record->dedicated.bytes;
   return record->start < RADV_PS5_RING_WORDS && record->start + n <= RADV_PS5_RING_WORDS;
}

static void
radv_ps5_dump_in_flight(const struct radv_ps5_queue *queue, const struct radv_ps5_in_flight *record, uint64_t completed)
{
   const uint32_t *words;
   uint32_t count;
   const bool scanned = radv_ps5_record_words(queue, record, &words, &count);
   const char *state = record->seq <= completed    ? "done"
                       : record->seq == completed + 1 ? "STUCK"
                                                      : "queued";
   struct radv_ps5_stream_scan scan;
   if (scanned)
      radv_ps5_scan_stream(words, count, &scan);
   fprintf(stderr,
           "radv/ps5 stall:   seq %" PRIu64 " %-6s dwords=%u agc_ibs=%u queue=%u waits=%u signals=%u depends=%" PRIu64
           " %s",
           record->seq, state, count, record->parts, record->queue_index, record->wait_count, record->signal_count,
           record->depends, record->dedicated.cpu ? "dedicated" : "ring");
   if (scanned) {
      fprintf(stderr, " packets=%u draws=%u dispatches=%u gpu_waits=%u indirect=%u cond=%u completion=%s%s last:",
              scan.packets, scan.draws, scan.dispatches, scan.waits, scan.indirect, scan.conditional,
              scan.ends_with_release_mem ? "ok" : "MISSING", scan.truncated ? " TRUNCATED" : "");
      for (uint32_t i = 0; i < scan.last_count; i++) {
         const char *name = radv_ps5_pkt3_name(scan.last_op[i]);
         if (name)
            fprintf(stderr, " %s", name);
         else
            fprintf(stderr, " 0x%02x", scan.last_op[i]);
      }
   } else {
      fprintf(stderr, " (words out of range, not scanned)");
   }
   fprintf(stderr, "\n");
}

/* ------------------------------------------------- whose code sits at a GPU address */

/* Every shader pipeline creation made and every vertex-input prolog, newest last, in a ring that
 * overwrites its oldest entries once full. Writers claim a slot with one atomic increment; the
 * reader is the stall dump, ten seconds after anything was written. */
#define RADV_PS5_SHADER_NOTES 65536

struct radv_ps5_shader_note {
   uint64_t va;
   uint64_t pipeline_hash;
   uint64_t pipeline;
   const char *stage;
   uint32_t bytes;
   uint32_t kind;
};

static struct radv_ps5_shader_note radv_ps5_shader_notes[RADV_PS5_SHADER_NOTES];
static uint32_t radv_ps5_shader_note_count;

void
radv_ps5_note_shader(uint64_t va, uint32_t bytes, const char *stage, uint32_t kind, uint64_t pipeline_hash,
                     uint64_t pipeline)
{
   if (!va || !bytes)
      return;
   const uint32_t slot = (p_atomic_inc_return(&radv_ps5_shader_note_count) - 1) % RADV_PS5_SHADER_NOTES;
   radv_ps5_shader_notes[slot] = (struct radv_ps5_shader_note){
      .va = va,
      .pipeline_hash = pipeline_hash,
      .pipeline = pipeline,
      .stage = stage ? stage : "?",
      .bytes = bytes,
      .kind = kind,
   };
}

/* Whose code is at address, newest first. Up to four are named: pipelines share a shader (RADV's
 * shader cache), and code freed with its pipeline leaves its entry behind for the next code put
 * at that address, so the newest entry is the likeliest. */
void eden_gpu_stalled(void) __attribute__((weak));

static void
radv_ps5_dump_shader_owner(const char *reg, uint64_t address)
{
   const uint32_t total = p_atomic_read(&radv_ps5_shader_note_count);
   const uint32_t kept = total < RADV_PS5_SHADER_NOTES ? total : RADV_PS5_SHADER_NOTES;
   uint32_t found = 0;
   for (uint32_t i = 0; i < kept && found < 4; i++) {
      const struct radv_ps5_shader_note *const note = &radv_ps5_shader_notes[(total - 1 - i) % RADV_PS5_SHADER_NOTES];
      if (address < note->va || address - note->va >= note->bytes)
         continue;
      const uint32_t kind = note->kind & ~RADV_PS5_SHADER_INTERNAL;
      if (kind == RADV_PS5_SHADER_PROLOG) {
         if (!found)
            fprintf(stderr,
                    "radv/ps5 stall: %s 0x%" PRIx64 " = a vertex-input prolog (RADV's own; the draw's vertex shader "
                    "runs after it, in the pipeline its PS names)\n",
                    reg, address);
         found++;
         break;
      }
      fprintf(stderr, "radv/ps5 stall: %s 0x%" PRIx64 " = pipeline %016" PRIx64 " vk=0x%" PRIx64 " (%s %s%s%s)\n", reg,
              address, note->pipeline_hash, note->pipeline, kind == RADV_PS5_SHADER_COMPUTE ? "compute" : "graphics",
              note->stage, (note->kind & RADV_PS5_SHADER_INTERNAL) ? ", RADV internal" : "",
              found ? ", recorded earlier at this address" : "");
      found++;
   }
   if (!found)
      fprintf(stderr, "radv/ps5 stall: %s 0x%" PRIx64 " = no recorded shader (%u recorded, the newest %u kept)\n", reg,
              address, total, kept);
}

/* ------------------------------------------------------- the stuck submission, in depth */

#define RADV_PS5_STALL_WAITS 256
#define RADV_PS5_STALL_WRITES 1024
#define RADV_PS5_STALL_WORK 16
#define RADV_PS5_STALL_WAIT_LINES 64

/* The program address registers the walk follows (GFX10.3: the ES registers hold the NGG or the
 * legacy GS stage's program, the LS registers the HS stage's), in pipeline order. */
enum {
   RADV_PS5_STAGE_VS,
   RADV_PS5_STAGE_HS,
   RADV_PS5_STAGE_GS,
   RADV_PS5_STAGE_PS,
   RADV_PS5_STAGE_CS,
   RADV_PS5_STAGES,
};
static const char *const radv_ps5_stage_names[RADV_PS5_STAGES] = {"VS", "HS", "GS", "PS", "CS"};

enum radv_ps5_wait_state {
   RADV_PS5_WAIT_MET,         /* its condition holds now */
   RADV_PS5_WAIT_PASSED,      /* it does not, but a later write in the submission left the value */
   RADV_PS5_WAIT_UNMET,       /* it does not, and nothing later in the submission explains why */
   RADV_PS5_WAIT_UNKNOWN,     /* the CPU cannot see what it waits on */
   RADV_PS5_WAIT_CONDITIONAL, /* a COND_EXEC: no wait, a run of dwords it may skip */
};

struct radv_ps5_stall_wait {
   uint32_t at;      /* the packet's first dword in the submission */
   uint32_t op;
   uint32_t number;  /* 1 for the submission's first wait; 0 for a COND_EXEC */
   uint32_t control; /* WAIT_REG_MEM's function and space; COND_EXEC's dword count */
   uint32_t ref;
   uint32_t mask;
   uint32_t value; /* what the CPU reads at address now, if readable */
   uint64_t address; /* memory, or a register's dword offset */
   bool memory;
   bool readable;
   uint8_t state;
};

struct radv_ps5_stall_write {
   uint32_t at;
   uint32_t value;
   uint64_t address;
};

struct radv_ps5_stall_work {
   uint32_t at;
   uint32_t op;
   uint32_t ordinal;
   uint64_t address[RADV_PS5_STAGES];
   /* How much work it asked for: a direct dispatch's groups in x, y and z; a direct draw's vertex or
    * index count, and its instances when a NUM_INSTANCES before it in this submission set them (0:
    * not known here). An indirect draw's are in memory: its first draw's count and instances as the
    * CPU reads them now (args_read), and for a multi draw how many draws it makes (draws, 0: one). */
   uint32_t size[3];
   uint32_t instances;
   bool args_read;
   bool args_unreadable;
   uint32_t draws;
};

/* The packet at words[i] (i < count): its opcode and its length in dwords (a type-2 filler is one
 * dword with no opcode). False at a word that starts no packet, or a packet that runs past count. */
static bool
radv_ps5_stall_packet(const uint32_t *words, uint32_t count, uint32_t i, uint32_t *op, uint32_t *n)
{
   const uint32_t header = words[i];
   if (PKT_TYPE_G(header) == 2) {
      *op = UINT32_MAX;
      *n = 1;
      return true;
   }
   if (PKT_TYPE_G(header) != 3)
      return false;
   *op = PKT3_IT_OPCODE_G(header);
   const uint32_t body = PKT_COUNT_G(header);
   *n = (*op == PKT3_NOP && body == 0x3fff) ? 1 : body + 2;
   return *n <= count - i;
}

/* WAIT_REG_MEM's FUNCTION field. */
static const char *const radv_ps5_wait_functions[8] = {"always", "<", "<=", "==", "!=", ">=", ">", "reserved"};

static bool
radv_ps5_stall_holds(uint32_t function, uint32_t value, uint32_t ref)
{
   switch (function) {
   case 0: return true;
   case 1: return value < ref;
   case 2: return value <= ref;
   case 3: return value == ref;
   case 4: return value != ref;
   case 5: return value >= ref;
   case 6: return value > ref;
   default: return false;
   }
}

/* Which program address registers a draw runs, by VGT_SHADER_STAGES_EN: every one when the walk
 * has not seen it. With NGG (PRIMGEN_EN) the VS stage does not run; the GS stage runs the vertex
 * or tessellation evaluation shader. */
static bool
radv_ps5_stall_stage_runs(unsigned stage, bool known, uint32_t stages)
{
   if (!known)
      return true;
   switch (stage) {
   case RADV_PS5_STAGE_VS: return !G_028B54_PRIMGEN_EN(stages);
   case RADV_PS5_STAGE_HS: return G_028B54_HS_EN(stages);
   case RADV_PS5_STAGE_GS: return G_028B54_ES_EN(stages) || G_028B54_GS_EN(stages) || G_028B54_PRIMGEN_EN(stages);
   default: return true;
   }
}

/* The program address register (byte offset) reg is, as a stage and whether it is the high half;
 * -1 for any other register. */
static int
radv_ps5_stall_program_reg(uint32_t reg, bool *high)
{
   *high = false;
   switch (reg) {
   case R_00B124_SPI_SHADER_PGM_HI_VS: *high = true; return RADV_PS5_STAGE_VS;
   case R_00B120_SPI_SHADER_PGM_LO_VS: return RADV_PS5_STAGE_VS;
   case R_00B524_SPI_SHADER_PGM_HI_LS: *high = true; return RADV_PS5_STAGE_HS;
   case R_00B520_SPI_SHADER_PGM_LO_LS: return RADV_PS5_STAGE_HS;
   case R_00B324_SPI_SHADER_PGM_HI_ES: *high = true; return RADV_PS5_STAGE_GS;
   case R_00B320_SPI_SHADER_PGM_LO_ES: return RADV_PS5_STAGE_GS;
   case R_00B024_SPI_SHADER_PGM_HI_PS: *high = true; return RADV_PS5_STAGE_PS;
   case R_00B020_SPI_SHADER_PGM_LO_PS: return RADV_PS5_STAGE_PS;
   case R_00B834_COMPUTE_PGM_HI: *high = true; return RADV_PS5_STAGE_CS;
   case R_00B830_COMPUTE_PGM_LO: return RADV_PS5_STAGE_CS;
   default: return -1;
   }
}

static void
radv_ps5_dump_wait_text(const struct radv_ps5_stall_wait *w)
{
   if (w->op == PKT3_WAIT_ON_CE_COUNTER) {
      fprintf(stderr, "WAIT_ON_CE_COUNTER");
   } else if (w->op == PKT3_COND_EXEC) {
      fprintf(stderr, "COND_EXEC mem 0x%" PRIx64 " (the next %u dwords run if it is not 0)", w->address, w->control);
   } else if (w->memory) {
      fprintf(stderr, "WAIT_REG_MEM mem 0x%" PRIx64 " & 0x%08x %s 0x%08x", w->address, w->mask,
              radv_ps5_wait_functions[w->control & 0x7], w->ref);
   } else {
      fprintf(stderr, "WAIT_REG_MEM reg 0x%05" PRIx64 " & 0x%08x %s 0x%08x", w->address * 4, w->mask,
              radv_ps5_wait_functions[w->control & 0x7], w->ref);
   }
   if (w->readable)
      fprintf(stderr, ", now 0x%08x", w->value);
}

static void
radv_ps5_dump_wait_state(const struct radv_ps5_stall_wait *w, bool reached, bool stop)
{
   if (!reached) {
      fprintf(stderr, "not reached\n");
   } else if (stop) {
      fprintf(stderr, "UNMET, the first: the GPU stops here\n");
   } else if (w->state == RADV_PS5_WAIT_CONDITIONAL) {
      if (!w->readable)
         fprintf(stderr, "cannot tell: the CPU cannot read that memory\n");
      else
         fprintf(stderr, "%s\n", w->value ? "runs" : "skips");
   } else if (w->state == RADV_PS5_WAIT_MET) {
      fprintf(stderr, "met\n");
   } else if (w->state == RADV_PS5_WAIT_PASSED) {
      fprintf(stderr, "passed (a later write in this submission left that value)\n");
   } else if (w->op == PKT3_WAIT_ON_CE_COUNTER) {
      fprintf(stderr, "cannot tell: the constant engine's counter\n");
   } else if (!w->memory) {
      fprintf(stderr, "cannot tell: a register\n");
   } else {
      fprintf(stderr, "cannot tell: the CPU cannot read that memory\n");
   }
}

/* The stuck submission, packet by packet: the waits it carries and what each reads now, the first
 * whose condition does not hold (where the in-order GPU stopped), the last draws and dispatches
 * before it with the program each runs, and whose code that is. */
static void
radv_ps5_dump_stuck(const uint32_t *words, uint32_t count, uint64_t seq)
{
   static struct radv_ps5_stall_wait waits[RADV_PS5_STALL_WAITS];
   static struct radv_ps5_stall_write writes[RADV_PS5_STALL_WRITES];
   static struct radv_ps5_stall_work draws[RADV_PS5_STALL_WORK], dispatches[RADV_PS5_STALL_WORK];
   uint32_t wait_count = 0, wait_total = 0, cond_total = 0, write_count = 0;
   uint32_t at = 0, op, n;

   /* 1. The waits, the conditional runs and the memory writes (WRITE_DATA and RELEASE_MEM: what
    *    explains a counter a wait has been passed on), in order. */
   for (; at < count && radv_ps5_stall_packet(words, count, at, &op, &n); at += n) {
      const uint32_t *const body = words + at + 1;
      const uint32_t body_dw = n - 1;
      if (op == PKT3_WAIT_REG_MEM || op == PKT3_WAIT_ON_CE_COUNTER || op == PKT3_COND_EXEC) {
         const uint32_t number = op == PKT3_COND_EXEC ? 0 : ++wait_total;
         cond_total += op == PKT3_COND_EXEC;
         if (wait_count == RADV_PS5_STALL_WAITS)
            continue;
         struct radv_ps5_stall_wait *const w = &waits[wait_count];
         memset(w, 0, sizeof(*w));
         w->at = at;
         w->op = op;
         w->number = number;
         if (op == PKT3_WAIT_REG_MEM && body_dw >= 5) {
            w->control = body[0];
            w->memory = ((body[0] >> 4) & 0x3) == 1;
            w->address = w->memory ? (((uint64_t)body[2] << 32) | (body[1] & ~3u)) : body[1];
            w->ref = body[3];
            w->mask = body[4];
         } else if (op == PKT3_COND_EXEC && body_dw >= 4) {
            w->memory = true;
            w->address = ((uint64_t)body[1] << 32) | (body[0] & ~3u);
            w->control = body[3] & 0x3fff;
         } else if (op != PKT3_WAIT_ON_CE_COUNTER) {
            continue; /* too short to be what it says */
         }
         wait_count++;
      } else if (op == PKT3_WRITE_DATA && body_dw >= 4) {
         /* DST_SEL: memory (1 synchronised across GRBM, 5), or L2 (2). */
         const uint32_t dst = (body[0] >> 8) & 0xf;
         const uint64_t address = ((uint64_t)body[2] << 32) | (body[1] & ~3u);
         for (uint32_t k = 3; (dst == 1 || dst == 2 || dst == 5) && k < body_dw && k < 7 &&
                              write_count < RADV_PS5_STALL_WRITES;
              k++)
            writes[write_count++] =
               (struct radv_ps5_stall_write){.at = at, .value = body[k], .address = address + 4 * (k - 3)};
      } else if (op == PKT3_RELEASE_MEM && body_dw >= 6) {
         /* DST_SEL (17:16) memory or L2; DATA_SEL (31:29) the 32- or 64-bit value. */
         const uint32_t dst = (body[1] >> 16) & 0x3, data = body[1] >> 29;
         const uint64_t address = ((uint64_t)body[3] << 32) | (body[2] & ~3u);
         for (uint32_t k = 0; dst <= 1 && (data == 1 || data == 2) && k < data && write_count < RADV_PS5_STALL_WRITES;
              k++)
            writes[write_count++] =
               (struct radv_ps5_stall_write){.at = at, .value = body[4 + k], .address = address + 4 * k};
      }
   }
   const uint32_t parsed = at;

   /* 2. What each reads now, and the first wait whose condition does not hold. A wait on a counter
    *    the submission counts up does not hold once the counter has moved past it either; a later
    *    write of the value it reads now says the GPU got past it. */
   int stop_wait = -1;
   uint32_t unknown = 0;
   for (uint32_t k = 0; k < wait_count; k++) {
      struct radv_ps5_stall_wait *const w = &waits[k];
      if (w->memory)
         w->readable = radv_ps5_stall_read(w->address, &w->value);
      if (w->op == PKT3_COND_EXEC) {
         w->state = RADV_PS5_WAIT_CONDITIONAL;
      } else if (!w->readable) {
         w->state = RADV_PS5_WAIT_UNKNOWN;
         unknown++;
      } else if (radv_ps5_stall_holds(w->control & 0x7, w->value & w->mask, w->ref)) {
         w->state = RADV_PS5_WAIT_MET;
      } else {
         w->state = RADV_PS5_WAIT_UNMET;
         for (uint32_t j = 0; j < write_count; j++) {
            if (writes[j].at > w->at && writes[j].address == w->address && writes[j].value == w->value) {
               w->state = RADV_PS5_WAIT_PASSED;
               break;
            }
         }
         if (w->state == RADV_PS5_WAIT_UNMET && stop_wait < 0)
            stop_wait = (int)k;
      }
   }
   const uint32_t stop = stop_wait >= 0 ? waits[stop_wait].at : parsed;

   /* 3. The draws and dispatches before that point, with the program each runs as the SET_SH_REG
    *    packets before it left the program address registers. */
   uint32_t lo[RADV_PS5_STAGES] = {0}, hi[RADV_PS5_STAGES] = {0};
   uint32_t stages = 0, draw_total = 0, dispatch_total = 0, instances = 0;
   uint64_t indirect_base = 0; /* SET_BASE 1: where an indirect draw's data_offset points into */
   bool stages_known = false;
   for (at = 0; at < stop && radv_ps5_stall_packet(words, count, at, &op, &n); at += n) {
      const uint32_t *const body = words + at + 1;
      const uint32_t body_dw = n - 1;
      if ((op == PKT3_SET_SH_REG || op == PKT3_SET_SH_REG_INDEX) && body_dw >= 2) {
         const uint32_t reg = SI_SH_REG_OFFSET + (body[0] & 0xffff) * 4;
         for (uint32_t k = 1; k < body_dw; k++) {
            bool high;
            const int stage = radv_ps5_stall_program_reg(reg + (k - 1) * 4, &high);
            if (stage >= 0)
               (high ? hi : lo)[stage] = body[k];
         }
      } else if (op == PKT3_SET_CONTEXT_REG && body_dw >= 2) {
         const uint32_t reg = SI_CONTEXT_REG_OFFSET + (body[0] & 0xffff) * 4;
         if (R_028B54_VGT_SHADER_STAGES_EN >= reg && R_028B54_VGT_SHADER_STAGES_EN < reg + (body_dw - 1) * 4) {
            stages = body[1 + (R_028B54_VGT_SHADER_STAGES_EN - reg) / 4];
            stages_known = true;
         }
      } else if (op == PKT3_NUM_INSTANCES && body_dw >= 1) {
         instances = body[0];
      } else if (op == PKT3_SET_BASE && body_dw >= 3 && (body[0] & 0xf) == 1) {
         indirect_base = body[1] | ((uint64_t)body[2] << 32);
      } else if (radv_ps5_pkt3_is_draw(op) || op == PKT3_DISPATCH_DIRECT || op == PKT3_DISPATCH_INDIRECT) {
         const bool draw = radv_ps5_pkt3_is_draw(op);
         const uint32_t ordinal = draw ? ++draw_total : ++dispatch_total;
         struct radv_ps5_stall_work *const slot = &(draw ? draws : dispatches)[(ordinal - 1) % RADV_PS5_STALL_WORK];
         memset(slot, 0, sizeof(*slot));
         slot->at = at;
         slot->op = op;
         slot->ordinal = ordinal;
         if (op == PKT3_DISPATCH_DIRECT && body_dw >= 3) {
            slot->size[0] = body[0];
            slot->size[1] = body[1];
            slot->size[2] = body[2];
         } else if (draw) {
            /* DRAW_INDEX_AUTO: count, initiator. DRAW_INDEX_2: max, base lo, base hi, count, initiator.
             * DRAW_INDEX_OFFSET_2: max, offset, count, initiator. */
            if (op == PKT3_DRAW_INDEX_AUTO && body_dw >= 1)
               slot->size[0] = body[0];
            else if (op == PKT3_DRAW_INDEX_2 && body_dw >= 4)
               slot->size[0] = body[3];
            else if (op == PKT3_DRAW_INDEX_OFFSET_2 && body_dw >= 3)
               slot->size[0] = body[2];
            slot->instances = instances;
            /* DRAW_(INDEX_)INDIRECT(_MULTI): data_offset first; a multi draw then has its most draws
             * at body[4], and the address of its count at body[5..6] when COUNT_INDIRECT_ENABLE (bit
             * 30 of body[3]) is set. The first draw's arguments open with its count and instances. */
            const bool indirect = op == PKT3_DRAW_INDIRECT || op == PKT3_DRAW_INDEX_INDIRECT ||
                                  op == PKT3_DRAW_INDIRECT_MULTI || op == PKT3_DRAW_INDEX_INDIRECT_MULTI;
            if (indirect && indirect_base && body_dw >= 1) {
               const uint64_t args = indirect_base + body[0];
               slot->instances = 0;
               slot->args_read = radv_ps5_stall_read(args, &slot->size[0]) &&
                                 radv_ps5_stall_read(args + 4, &slot->instances);
               slot->args_unreadable = !slot->args_read;
               if (slot->args_read && (op == PKT3_DRAW_INDIRECT_MULTI || op == PKT3_DRAW_INDEX_INDIRECT_MULTI) &&
                   body_dw >= 7) {
                  slot->draws = body[4];
                  const uint64_t count_va = body[5] | ((uint64_t)body[6] << 32);
                  uint32_t draw_count;
                  if ((body[3] & (1u << 30)) && count_va && radv_ps5_stall_read(count_va, &draw_count) &&
                      draw_count < slot->draws)
                     slot->draws = draw_count;
               }
            }
         }
         for (unsigned s = 0; s < RADV_PS5_STAGES; s++) {
            const bool runs = draw ? s != RADV_PS5_STAGE_CS && radv_ps5_stall_stage_runs(s, stages_known, stages)
                                   : s == RADV_PS5_STAGE_CS;
            if (runs && lo[s])
               slot->address[s] = ((uint64_t)lo[s] << 8) | ((uint64_t)(hi[s] & 0xff) << 40);
         }
      }
   }

   /* What matters most goes first: the console may be gone before the rest is out. */
   if (stop_wait >= 0) {
      const struct radv_ps5_stall_wait *const w = &waits[stop_wait];
      fprintf(stderr, "radv/ps5 stall: seq %" PRIu64 " stops at wait %u of %u, dword %u: ", seq, w->number, wait_total,
              w->at);
      radv_ps5_dump_wait_text(w);
      fprintf(stderr, "\n");
   } else {
      fprintf(stderr,
              "radv/ps5 stall: seq %" PRIu64 ": none of its %u waits reads as unmet (%u the CPU cannot tell)%s\n",
              seq, wait_total, unknown, parsed < count ? ", and its words stop parsing early" : "");
   }
   fprintf(stderr, "radv/ps5 stall: the last draws and dispatches before dword %u (%u draws, %u dispatches before it):\n",
           stop, draw_total, dispatch_total);
   const struct radv_ps5_stall_work *work[2 * RADV_PS5_STALL_WORK];
   uint32_t work_count = 0;
   for (uint32_t k = 0; k < RADV_PS5_STALL_WORK; k++) {
      if (k < draw_total)
         work[work_count++] = &draws[k];
      if (k < dispatch_total)
         work[work_count++] = &dispatches[k];
   }
   for (uint32_t k = 1; k < work_count; k++) { /* in stream order */
      const struct radv_ps5_stall_work *const item = work[k];
      uint32_t j = k;
      for (; j > 0 && work[j - 1]->at > item->at; j--)
         work[j] = work[j - 1];
      work[j] = item;
   }
   const char *owner_reg[2 * RADV_PS5_STALL_WORK * RADV_PS5_STAGES];
   uint64_t owner_address[2 * RADV_PS5_STALL_WORK * RADV_PS5_STAGES];
   uint32_t owners = 0;
   for (uint32_t k = 0; k < work_count; k++) {
      const struct radv_ps5_stall_work *const item = work[k];
      const bool draw = radv_ps5_pkt3_is_draw(item->op);
      fprintf(stderr, "radv/ps5 stall:   %s %u at dword %u %s", draw ? "draw" : "dispatch", item->ordinal, item->at,
              radv_ps5_pkt3_name(item->op));
      if (!draw && item->op == PKT3_DISPATCH_DIRECT)
         fprintf(stderr, " %ux%ux%u groups", item->size[0], item->size[1], item->size[2]);
      else if (draw && item->args_read) {
         fprintf(stderr, " args count %u x%u instances", item->size[0], item->instances);
         if (item->draws > 1)
            fprintf(stderr, ", the first of %u draws", item->draws);
      } else if (draw && item->args_unreadable)
         fprintf(stderr, " args unreadable");
      else if (draw && item->size[0])
         fprintf(stderr, " count %u", item->size[0]);
      if (draw && !item->args_read && item->size[0] && item->instances)
         fprintf(stderr, " x%u instances", item->instances);
      fprintf(stderr, ":");
      bool any = false;
      for (unsigned s = 0; s < RADV_PS5_STAGES; s++) {
         if (!item->address[s])
            continue;
         any = true;
         fprintf(stderr, " %s 0x%" PRIx64, radv_ps5_stage_names[s], item->address[s]);
         bool seen = false;
         for (uint32_t j = 0; j < owners && !seen; j++)
            seen = owner_address[j] == item->address[s] && owner_reg[j] == radv_ps5_stage_names[s];
         if (!seen) {
            owner_reg[owners] = radv_ps5_stage_names[s];
            owner_address[owners++] = item->address[s];
         }
      }
      fprintf(stderr, "%s\n", any ? "" : " no program address set");
   }
   for (uint32_t k = 0; k < owners; k++)
      radv_ps5_dump_shader_owner(owner_reg[k], owner_address[k]);
   fflush(stderr);

   fprintf(stderr, "radv/ps5 stall: seq %" PRIu64 " carries %u waits and %u conditional runs:\n", seq, wait_total,
           cond_total);
   const uint32_t lines = wait_count < RADV_PS5_STALL_WAIT_LINES ? wait_count : RADV_PS5_STALL_WAIT_LINES;
   for (uint32_t k = 0; k < lines; k++) {
      const struct radv_ps5_stall_wait *const w = &waits[k];
      if (w->number)
         fprintf(stderr, "radv/ps5 stall:   wait %u at dword %u: ", w->number, w->at);
      else
         fprintf(stderr, "radv/ps5 stall:   cond at dword %u: ", w->at);
      radv_ps5_dump_wait_text(w);
      fprintf(stderr, ": ");
      radv_ps5_dump_wait_state(w, stop_wait < 0 || (int)k <= stop_wait, (int)k == stop_wait);
   }
   if (wait_total + cond_total > lines)
      fprintf(stderr, "radv/ps5 stall:   (%u more not shown)\n", wait_total + cond_total - lines);
   fflush(stderr);
}

static void
radv_ps5_dump_stall(struct radv_ps5_queue *queue, uint64_t seq, uint64_t completed)
{
   /* Once per process: the console may hang right after, and a short burst is all it can flush. */
   static uint32_t dumped;
   if (p_atomic_cmpxchg(&dumped, 0u, 1u) != 0u)
      return;
   /* Before anything here can fault: the app learns its GPU stopped, so a crash in this dump
    * closes it as VK_ERROR_DEVICE_LOST does instead of starting it again on the stuck GPU
    * (headless/crash_report.cpp). Weak: a build without the app has no such hook. */
   if (eden_gpu_stalled)
      eden_gpu_stalled();
   /* A lock-free, best-effort snapshot: taking the submit lock could deadlock when the stall
    * trips inside radv_ps5_queue_claim, which already holds it, and at a ten-second hang the
    * submitting thread is quiescent anyway. Indices are masked, and radv_ps5_record_words
    * refuses a record whose words fall outside their buffer. */
   const uint32_t first = p_atomic_read(&queue->in_flight_first);
   const uint32_t total = p_atomic_read(&queue->in_flight_count);
   fprintf(stderr,
           "radv/ps5 stall: waiting for submission %" PRIu64 ", GPU at %" PRIu64 ", submitted %" PRIu64
           ", %u in flight (best-effort snapshot)\n",
           seq, completed, p_atomic_read(&queue->submitted_seq), total);
   const uint32_t window = total < 8 ? total : 8;
   for (uint32_t i = 0; i < window; i++)
      radv_ps5_dump_in_flight(queue, &queue->in_flight[(first + i) % RADV_PS5_MAX_IN_FLIGHT], completed);
   fflush(stderr);
   /* The stuck one in depth: the queue runs in order, so it is the one after the GPU's. */
   const uint32_t searched = total < RADV_PS5_MAX_IN_FLIGHT ? total : RADV_PS5_MAX_IN_FLIGHT;
   for (uint32_t i = 0; i < searched; i++) {
      const struct radv_ps5_in_flight *const record = &queue->in_flight[(first + i) % RADV_PS5_MAX_IN_FLIGHT];
      if (record->seq != completed + 1)
         continue;
      const uint32_t *words;
      uint32_t count;
      if (radv_ps5_record_words(queue, record, &words, &count))
         radv_ps5_dump_stuck(words, count, record->seq);
      break;
   }
}

void
radv_ps5_record_submission(struct radv_ps5_queue *queue, uint32_t words, uint32_t parts, uint32_t wait_count,
                           uint32_t signal_count, uint32_t queue_index, uint64_t depends)
{
   if (!queue->in_flight_count)
      return;
   struct radv_ps5_in_flight *const record =
      &queue->in_flight[(queue->in_flight_first + queue->in_flight_count - 1) % RADV_PS5_MAX_IN_FLIGHT];
   record->words = words;
   record->parts = (uint16_t)parts;
   record->wait_count = (uint16_t)wait_count;
   record->signal_count = (uint16_t)signal_count;
   record->queue_index = (uint8_t)queue_index;
   record->depends = depends;
}

'''

patch(ps5 / 'radv_ps5_winsys.c', [
    (winsys_include_old, winsys_include_new, '#include <ps5platform/kernel.h> /* ProsperoEden'),
    ('bool\nradv_ps5_queue_wait_seq(struct radv_ps5_queue *queue, uint64_t seq, uint64_t abs_timeout_ns)\n{\n',
     stall_read_block + dump_block +
     'bool\nradv_ps5_queue_wait_seq(struct radv_ps5_queue *queue, uint64_t seq, uint64_t abs_timeout_ns)\n{\n',
     'radv_ps5_dump_stall(struct radv_ps5_queue *queue'),
    ('      } else if (now - progress_at > RADV_PS5_STALL_NS) {\n'
     '         fprintf(stderr, "radv/ps5: the GPU has not reached submission',
     '      } else if (now - progress_at > RADV_PS5_STALL_NS) {\n'
     '         radv_ps5_dump_stall(queue, seq, completed); /* ProsperoEden: tools/patch-radv-submission-dump.py */\n'
     '         fprintf(stderr, "radv/ps5: the GPU has not reached submission',
     'radv_ps5_dump_stall(queue, seq, completed);'),
])

# 4. The submit path records the metadata for the submission it just claimed, under the lock.
patch(ps5 / 'radv_ps5_cs.c', [
    ('   radv_ps5_note_submission(count, copy_start - claim_start, copy_end - copy_start);\n',
     '   radv_ps5_record_submission(queue, count, parts, wait_count, signal_count, submit->queue_index, depends);\n'
     '   radv_ps5_note_submission(count, copy_start - claim_start, copy_end - copy_start);\n',
     'radv_ps5_record_submission(queue, count, parts'),
])

# 5. Pipeline creation says where each shader went. Every graphics, graphics library and compute
#    pipeline RADV creates (the application's and its own meta pipelines) passes
#    radv_pipeline_report_pso_history once its shaders are uploaded, just before its handle is
#    returned.
ps5_includes = ('#if defined(RADV_PS5)\n'
                '#include "winsys/ps5/radv_ps5_winsys_public.h" /* ProsperoEden: tools/patch-radv-submission-dump.py */\n')
pipeline_include_old = '#include "vk_shader_module.h"\n'
pipeline_include_new = pipeline_include_old + ps5_includes + '#include "radv_gs_compute.h"\n#endif\n'

pipeline_note_block = r'''#if defined(RADV_PS5)
/* ProsperoEden (tools/patch-radv-submission-dump.py): where each of the pipeline's shaders sits on
 * the GPU, with the pipeline's hash and handle, so the winsys's stall dump can say whose code a
 * stuck draw or dispatch was running. */
static void
radv_ps5_note_pipeline_shaders(struct radv_pipeline *pipeline)
{
   /* RADV_GS_COMPUTE_VS onwards (radv_gs_compute_index). */
   static const char *const gs_compute_names[RADV_GS_COMPUTE_SHADERS] = {
      "GS-as-compute VS", "GS-as-compute COUNT", "GS-as-compute PRE_GS",
      "GS-as-compute MAIN", "GS-as-compute TCS", "GS-as-compute TES",
   };
   uint32_t kind;
   if (pipeline->type == RADV_PIPELINE_GRAPHICS || pipeline->type == RADV_PIPELINE_GRAPHICS_LIB)
      kind = RADV_PS5_SHADER_GRAPHICS;
   else if (pipeline->type == RADV_PIPELINE_COMPUTE)
      kind = RADV_PS5_SHADER_COMPUTE;
   else
      return;
   if (pipeline->is_internal)
      kind |= RADV_PS5_SHADER_INTERNAL;
   const uint64_t handle = (uint64_t)(uintptr_t)radv_pipeline_to_handle(pipeline);
   for (unsigned i = 0; i < MESA_VULKAN_SHADER_STAGES; i++) {
      const struct radv_shader *const shader = pipeline->shaders[i];
      if (shader)
         radv_ps5_note_shader(shader->va, shader->code_size, _mesa_shader_stage_to_abbrev(i), kind,
                              pipeline->pipeline_hash, handle);
   }
   if (pipeline->gs_copy_shader)
      radv_ps5_note_shader(pipeline->gs_copy_shader->va, pipeline->gs_copy_shader->code_size, "GS copy", kind,
                           pipeline->pipeline_hash, handle);
   for (unsigned i = 0; pipeline->gs_compute && i < RADV_GS_COMPUTE_SHADERS; i++) {
      const struct radv_shader *const shader = pipeline->gs_compute->shaders[i];
      if (shader)
         radv_ps5_note_shader(shader->va, shader->code_size, gs_compute_names[i], kind, pipeline->pipeline_hash,
                              handle);
   }
}
#endif

'''
report_old = ('void\nradv_pipeline_report_pso_history(const struct radv_device *device, struct radv_pipeline *pipeline)\n'
              '{\n')
report_new = (pipeline_note_block + report_old +
              '#if defined(RADV_PS5)\n'
              '   radv_ps5_note_pipeline_shaders(pipeline); /* ProsperoEden: tools/patch-radv-submission-dump.py */\n'
              '#endif\n')

patch(vulkan / 'radv_pipeline.c', [
    (pipeline_include_old, pipeline_include_new, '#include "winsys/ps5/radv_ps5_winsys_public.h" /* ProsperoEden'),
    (report_old, report_new, 'radv_ps5_note_pipeline_shaders(struct radv_pipeline *pipeline)'),
])

# 6. So do RADV's vertex-input prologs: with dynamic vertex input (which Eden uses) the program a
#    draw's vertex stage starts at is the prolog, which then jumps to the pipeline's shader.
shader_include_old = '#include "radv_shader.h"\n'
shader_include_new = shader_include_old + ps5_includes + '#endif\n'
prolog_old = '   prolog->nontrivial_divisors = key->nontrivial_divisors;\n'
prolog_new = (prolog_old +
              '#if defined(RADV_PS5)\n'
              '   /* ProsperoEden: tools/patch-radv-submission-dump.py */\n'
              '   radv_ps5_note_shader(prolog->va, prolog->code_size, "VS prolog",\n'
              '                        RADV_PS5_SHADER_PROLOG | RADV_PS5_SHADER_INTERNAL, 0, 0);\n'
              '#endif\n')

patch(vulkan / 'radv_shader.c', [
    (shader_include_old, shader_include_new, '#include "winsys/ps5/radv_ps5_winsys_public.h" /* ProsperoEden'),
    (prolog_old, prolog_new, 'radv_ps5_note_shader(prolog->va'),
])

if changed:
    # Upstream skips the whole build while its receipt names the pinned revision: set the receipt
    # aside so the next build compiles the changed files (Ninja keeps every other object).
    provenance = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print(f'RADV submission dump: changed={changed}')
