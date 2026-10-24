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

The threaded recording layer (radv_threaded_layer.c, which #277 points at) is compiled into the
driver but only runs when RADV_THREADED_RECORDING is set, which ProsperoEden never sets, so it
records nothing in a shipping build; naming the Vulkan command types would need a change above the
winsys and is not done here.

Applied to the driver checkout after patch-radv-transfer-split.py; running it again changes
nothing.
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
ps5 = src / 'src/amd/vulkan/winsys/ps5'
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

# 2. The dump itself, and the recorder, in the winsys, before the wait that detects the stall;
#    and the call from inside that wait when it trips.
dump_block = r'''/* ProsperoEden (tools/patch-radv-submission-dump.py): when a submission's words stop making
 * progress for RADV_PS5_STALL_NS, say what the stuck submission and the ones queued behind it
 * carry, before the console is gone. Nothing here runs until the ten-second wait trips. */

static const char *
radv_ps5_pkt3_name(uint32_t op)
{
   switch (op) {
   case PKT3_NOP: return "NOP";
   case PKT3_DRAW_INDEX_AUTO: return "DRAW_INDEX_AUTO";
   case PKT3_DRAW_INDEX_2: return "DRAW_INDEX_2";
   case PKT3_DRAW_INDEX_INDIRECT: return "DRAW_INDEX_INDIRECT";
   case PKT3_DRAW_INDEX_INDIRECT_MULTI: return "DRAW_INDEX_INDIRECT_MULTI";
   case PKT3_DISPATCH_DIRECT: return "DISPATCH_DIRECT";
   case PKT3_DISPATCH_INDIRECT: return "DISPATCH_INDIRECT";
   case PKT3_WAIT_REG_MEM: return "WAIT_REG_MEM";
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
      switch (op) {
      case PKT3_DRAW_INDEX_AUTO:
      case PKT3_DRAW_INDEX_2: scan->draws++; break;
      case PKT3_DRAW_INDEX_INDIRECT:
      case PKT3_DRAW_INDEX_INDIRECT_MULTI: scan->draws++; scan->indirect++; break;
      case PKT3_DISPATCH_DIRECT: scan->dispatches++; break;
      case PKT3_DISPATCH_INDIRECT: scan->dispatches++; scan->indirect++; break;
      case PKT3_WAIT_REG_MEM: scan->waits++; break;
      case PKT3_COND_EXEC:
      case PKT3_COND_WRITE: scan->conditional++; break;
      case PKT3_INDIRECT_BUFFER: scan->indirect++; break;
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

static void
radv_ps5_dump_in_flight(const struct radv_ps5_queue *queue, const struct radv_ps5_in_flight *record, uint64_t completed)
{
   const bool dedicated = record->dedicated.cpu != NULL;
   uint32_t count = record->words;
   if (!count && !dedicated)
      count = record->end - record->start;
   const uint32_t *words = dedicated ? (const uint32_t *)record->dedicated.cpu
                                     : (const uint32_t *)queue->ring.cpu + record->start;
   const char *state = record->seq <= completed    ? "done"
                       : record->seq == completed + 1 ? "STUCK"
                                                      : "queued";
   struct radv_ps5_stream_scan scan;
   bool scanned = false;
   /* Only walk words that are inside the buffer they claim, so a record caught mid-claim cannot
    * send the scan out of bounds. */
   if (count && count <= RADV_PS5_RING_WORDS &&
       (dedicated || (record->start < RADV_PS5_RING_WORDS && record->start + count <= RADV_PS5_RING_WORDS))) {
      radv_ps5_scan_stream(words, count, &scan);
      scanned = true;
   }
   fprintf(stderr,
           "radv/ps5 stall:   seq %" PRIu64 " %-6s dwords=%u agc_ibs=%u queue=%u waits=%u signals=%u depends=%" PRIu64
           " %s",
           record->seq, state, count, record->parts, record->queue_index, record->wait_count, record->signal_count,
           record->depends, dedicated ? "dedicated" : "ring");
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

static void
radv_ps5_dump_stall(struct radv_ps5_queue *queue, uint64_t seq, uint64_t completed)
{
   /* Once per process: the console may hang right after, and a short burst is all it can flush. */
   static uint32_t dumped;
   if (p_atomic_cmpxchg(&dumped, 0u, 1u) != 0u)
      return;
   /* A lock-free, best-effort snapshot: taking the submit lock could deadlock when the stall
    * trips inside radv_ps5_queue_claim, which already holds it, and at a ten-second hang the
    * submitting thread is quiescent anyway. Indices are masked, and radv_ps5_dump_in_flight
    * refuses to scan a record whose words fall outside their buffer. */
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
    ('bool\nradv_ps5_queue_wait_seq(struct radv_ps5_queue *queue, uint64_t seq, uint64_t abs_timeout_ns)\n{\n',
     dump_block + 'bool\nradv_ps5_queue_wait_seq(struct radv_ps5_queue *queue, uint64_t seq, uint64_t abs_timeout_ns)\n{\n',
     'radv_ps5_dump_stall(struct radv_ps5_queue *queue'),
    ('      } else if (now - progress_at > RADV_PS5_STALL_NS) {\n'
     '         fprintf(stderr, "radv/ps5: the GPU has not reached submission',
     '      } else if (now - progress_at > RADV_PS5_STALL_NS) {\n'
     '         radv_ps5_dump_stall(queue, seq, completed); /* ProsperoEden: tools/patch-radv-submission-dump.py */\n'
     '         fprintf(stderr, "radv/ps5: the GPU has not reached submission',
     'radv_ps5_dump_stall(queue, seq, completed);'),
])

# 3. The submit path records the metadata for the submission it just claimed, under the lock.
patch(ps5 / 'radv_ps5_cs.c', [
    ('   radv_ps5_note_submission(count, copy_start - claim_start, copy_end - copy_start);\n',
     '   radv_ps5_record_submission(queue, count, parts, wait_count, signal_count, submit->queue_index, depends);\n'
     '   radv_ps5_note_submission(count, copy_start - claim_start, copy_end - copy_start);\n',
     'radv_ps5_record_submission(queue, count, parts'),
])

if changed:
    # Upstream skips the whole build while its receipt names the pinned revision: set the receipt
    # aside so the next build compiles the changed files (Ninja keeps every other object).
    provenance = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print(f'RADV submission dump: changed={changed}')
