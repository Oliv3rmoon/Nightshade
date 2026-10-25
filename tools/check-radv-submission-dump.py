#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The submission-dump patch: its stamp, its build wiring, and that its generated code compiles.

Three things are checked without a console or a full Mesa build:
  1. The digest stamp round-trips and a changed patch invalidates a cached driver.
  2. prepare-build.sh and build-radv-dependencies.sh carry its check and its digest, in order.
  3. The C the patch inserts into the winsys compiles on its own, against a minimal shim of the
     winsys types and the PM4 packet macros it uses (the same values as src/amd/common/sid.h).
If a C compiler is not on PATH, step 3 is skipped with a note rather than failing.
"""
from pathlib import Path
import ast
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

# 3. The inserted winsys code compiles in isolation.
# Read the inserted C out of the patch's string literals without running its file edits.
tree = ast.parse(patch.read_text())
literals = {}
for node in tree.body:
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
        try:
            literals[node.targets[0].id] = ast.literal_eval(node.value)
        except (ValueError, TypeError):
            pass
header_fields_new = literals['header_fields_new']
dump_block = literals['dump_block']

cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
if not cc:
    print('PASS: stamp and build wiring (no C compiler on PATH; skipped the isolation compile)')
    raise SystemExit(0)

prologue = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

/* The PM4 encoding the dump decodes, with the values of src/amd/common/sid.h. */
#define PKT_TYPE_G(x)       (((x) >> 30) & 0x3)
#define PKT_COUNT_G(x)      (((x) >> 16) & 0x3FFF)
#define PKT3_IT_OPCODE_G(x) (((x) >> 8) & 0xFF)
#define PKT3_NOP                       0x10
#define PKT3_DISPATCH_DIRECT           0x15
#define PKT3_DISPATCH_INDIRECT         0x16
#define PKT3_DRAW_INDEX_INDIRECT       0x25
#define PKT3_DRAW_INDEX_2              0x27
#define PKT3_DRAW_INDEX_AUTO           0x2D
#define PKT3_WRITE_DATA                0x37
#define PKT3_DRAW_INDEX_INDIRECT_MULTI 0x38
#define PKT3_COND_EXEC                 0x22
#define PKT3_WAIT_REG_MEM              0x3C
#define PKT3_INDIRECT_BUFFER           0x3F
#define PKT3_COND_WRITE                0x45
#define PKT3_EVENT_WRITE               0x46
#define PKT3_RELEASE_MEM               0x49
#define PKT3_DMA_DATA                  0x50
#define PKT3_ACQUIRE_MEM               0x58

#define p_atomic_read(v) (*(v))
#define p_atomic_cmpxchg(v, old, _new) (*(v) == (old) ? (*(v) = (_new), (old)) : *(v))

/* A minimal stand-in for the winsys types the dump touches. */
struct radv_ps5_memory { uint8_t *cpu; uint64_t bytes; int64_t physical; };
#define RADV_PS5_MAX_IN_FLIGHT 4096
#define RADV_PS5_RING_WORDS    ((uint32_t)((16u << 20) / 4))
'''

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
# A user of every inserted function, so none is dropped and all are type-checked end to end.
driver = r'''
int main(void)
{
   static struct radv_ps5_queue queue;
   radv_ps5_record_submission(&queue, 0, 0, 0, 0, 0, 0);
   radv_ps5_dump_stall(&queue, 1, 0);
   return 0;
}
'''

source = prologue + struct_in_flight + struct_queue + dump_block + driver
with tempfile.TemporaryDirectory() as folder:
    c = Path(folder) / 'submission_dump_isolated.c'
    c.write_text(source)
    result = subprocess.run([cc, '-std=c11', '-c', '-Wall', '-Wno-unused-function', '-o',
                             str(Path(folder) / 'out.o'), str(c)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit('the inserted winsys code did not compile in isolation')

print('PASS: stamp, build wiring, and the inserted winsys code compiles in isolation')
