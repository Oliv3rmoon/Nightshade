#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of Tears of the Kingdom's bounded loop (headless/totk_guard.h) and of the module
names the freeze watchdog writes addresses with (headless/guest_progress.h, NoteModule).

Usage: check-totk-guard.py [EDEN_SOURCE]

- The 32 words the guard expects at main+0x6E13C4 are the loop as the watchdog read it on the
  console (v1.010.010, EDEN_STUCK_CODE), and the 14 it writes are the bounded loop: both are
  assembled here from their listings (clang, for AArch64) and must match the header word for word.
- Apply patches only that build, only when every word matches, and arms the system call's report
  only then; the report is one line for the first 8 and then for every power of two.
- A module noted by the loader names an address (main+0x6e1404, not 0x80a03404), and a new process
  noting its modules again starts a new list even where the last process lay.
- Where the host is AArch64, both loops run: for counts 0 to 4 they store the same floats in the
  same places; above 4 the original stores past the 4 floats its destination holds, the bounded
  one stores 4 and makes its system call once (counted here instead of made).
- With Eden's pinned source, the loader's step and PhysicalCore's system call are where the build
  writes them (headless/CMakeLists.txt).
"""
from pathlib import Path
import platform, re, shutil, struct, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'headless/totk_guard.h').read_text()
cmake = (root / 'headless/CMakeLists.txt').read_text()
failures = []


def words(name):
    body = re.search(r'%s\[\d+\] = \{(.*?)\};' % re.escape(name), header, re.S).group(1)
    return [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]{8}', body)]


original = words('kOriginal')
patched = words('kPatched')
assert len(original) == 32 and len(patched) == 14

ORIGINAL = r'''
    str s2, [x19, #0x2638]
    ldr s2, [x10, #0x4]
    fmadd s3, s2, s1, s0
    str s3, [x19, #0x263c]
    ldr s2, [x10, #0x8]
    mov w10, #0x58e0
    add x10, x9, x10
    fmadd s2, s2, s1, s0
    str s2, [x19, #0x2640]
    ldr w11, [x10]
    cbz w11, 3f
    mov w12, #0x2644
    mov w13, #0x5aa0
    mov x11, xzr
    add x12, x19, x12
    add x9, x9, x13
2:  ldr s2, [x9, x11, lsl #2]
    fmadd s2, s2, s1, s0
    str s2, [x12, x11, lsl #2]
    add x11, x11, #1
    ldr w13, [x10]
    cmp x11, x13
    b.lo 2b
3:  ldr w8, [x8]
    str w8, [x19, #0x2654]
    mov w8, #0x2af8
    add x22, x19, x8
    ldr x8, [x19, #0x2af8]
    mov x0, x22
    ldr x8, [x8, #0x10]
    blr x8
    tbz w0, #0, 4f
    .skip 0x24
4:
'''

PATCHED = r'''
    ldr  w11, [x10]
    add  x9, x10, #0x1c0
    mov  w12, #0x2644
    add  x12, x19, x12
    cmp  w11, #4
    b.ls 1f
    svc  #0x1e
    mov  w11, #4
1:  cbz  w11, 3f
2:  ldr  s2, [x9], #4
    fmadd s2, s2, s1, s0
    str  s2, [x12], #4
    subs w11, w11, #1
    b.ne 2b
3:
'''


def assemble(text, tmp, clang):
    source = tmp / 'listing.s'
    source.write_text('.text\n' + text)
    obj = tmp / 'listing.o'
    subprocess.run([clang, '-target', 'aarch64-linux-gnu', '-c', str(source), '-o', str(obj)], check=True,
                   capture_output=True)
    data = obj.read_bytes()
    shoff = struct.unpack_from('<Q', data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    names = sections[shstrndx]
    for section in sections:
        at = names[4] + section[0]
        if data[at:data.index(b'\0', at)] == b'.text':
            return list(struct.unpack_from('<%dI' % (section[5] // 4), data, section[4]))
    raise RuntimeError('no .text')


def check_listings(clang, tmp):
    try:
        listed = assemble(ORIGINAL, tmp, clang)[:32]
        bounded = assemble(PATCHED, tmp, clang)
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f'totk guard: no AArch64 assembler here ({error}); listings not compared')
        return
    for i, (want, got) in enumerate(zip(listed, original)):
        if want != got:
            failures.append(f'kOriginal[{i}] is {got:#010x}, the listing assembles to {want:#010x}')
    for i, (want, got) in enumerate(zip(bounded, patched)):
        if want != got:
            failures.append(f'kPatched[{i}] is {got:#010x}, the listing assembles to {want:#010x}')
    if len(bounded) != len(patched):
        failures.append(f'the bounded listing is {len(bounded)} words, kPatched {len(patched)}')
    # Its system call returns to main+0x6E1404, and its branch out lands where the original's did
    # (main+0x6E1420, word 23 of the window).
    svc = patched.index(0xd40003c1)
    if 0x6E13E8 + 4 * (svc + 1) != 0x6E1404 or 'kTrapReturn = kPatchAt + 7 * 4' not in header:
        failures.append('the system call does not return to main+0x6E1404')
    print(f'totk guard: {len(listed)} original and {len(bounded)} bounded words match their listings')


TEST = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "guest_progress.h"
#include "totk_guard.h"

using namespace Eden;
static int failed = 0;
#define EXPECT(condition) do { if (!(condition)) { std::fprintf(stdout, "FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); failed = 1; } } while (0)

int main() {
    const std::uint64_t base = 0x80322000;
    std::vector<std::uint8_t> image(0x700000, 0);
    for (std::size_t i = 0; i < 32; ++i) TotkGuard::PutWord(image.data() + TotkGuard::kWindow + 4 * i, TotkGuard::kOriginal[i]);
    std::uint8_t other[32] = {0x27, 0x71, 0x78, 0xB7, 0xDB, 0xA1, 0xB6, 0xD5};
    std::uint8_t totk[32] = {0x27, 0x71, 0x78, 0xB7, 0xDB, 0xA1, 0xB6, 0xD4};
    // Another build: untouched, not armed.
    EXPECT(TotkGuard::Apply(image.data(), image.size(), other, base) == TotkGuard::Applied::kOtherBuild);
    EXPECT(TotkGuard::PatchedBase().load() == 0);
    EXPECT(TotkGuard::Word(image.data() + 0x6E1404) == 0xbc6b7922);
    // This build with one word different: untouched, not armed.
    TotkGuard::PutWord(image.data() + 0x6E1430, 0xd503201f);
    EXPECT(TotkGuard::Apply(image.data(), image.size(), totk, base) == TotkGuard::Applied::kCodeDiffers);
    EXPECT(TotkGuard::PatchedBase().load() == 0);
    EXPECT(TotkGuard::Word(image.data() + 0x6E13EC) == 0x340001ab);
    TotkGuard::PutWord(image.data() + 0x6E1430, TotkGuard::kOriginal[27]);
    // Too small an image: untouched.
    EXPECT(TotkGuard::Apply(image.data(), 0x6E1000, totk, base) == TotkGuard::Applied::kCodeDiffers);
    // This build: the 14 words replaced, nothing else, armed at its base.
    std::vector<std::uint8_t> before = image;
    EXPECT(TotkGuard::Apply(image.data(), image.size(), totk, base) == TotkGuard::Applied::kPatched);
    EXPECT(TotkGuard::PatchedBase().load() == base);
    for (std::size_t i = 0; i < 14; ++i) EXPECT(TotkGuard::Word(image.data() + 0x6E13E8 + 4 * i) == TotkGuard::kPatched[i]);
    for (std::size_t at = 0; at < image.size(); ++at)
        if (at < 0x6E13E8 || at >= 0x6E1420) EXPECT(image[at] == before[at]);
    EXPECT(base + TotkGuard::kTrapReturn == 0x80a03404);
    // Applied twice (a second load of the same image): the words differ now, so nothing changes and
    // the session is not armed by it.
    EXPECT(TotkGuard::Apply(image.data(), image.size(), totk, base) == TotkGuard::Applied::kCodeDiffers);
    EXPECT(TotkGuard::PatchedBase().load() == 0);
    TotkGuard::ReportApplied(TotkGuard::Applied::kPatched, base);
    TotkGuard::ReportApplied(TotkGuard::Applied::kCodeDiffers, base);
    TotkGuard::ReportApplied(TotkGuard::Applied::kOtherBuild, base);
    // The report: the first 8, then powers of two.
    TotkGuard::Trap trap;
    trap.count = 940309209;
    trap.count_at = 0x217b5a08e0;
    trap.src = 0x217b5a0aa0;
    trap.dst = 0x217b590be0;
    trap.thread = 79;
    trap.page = "gpu-cached";
    trap.words_read = true;
    for (unsigned i = 0; i < 16; ++i) trap.words[i] = 0x380bfad9 + i;
    for (int i = 0; i < 40; ++i) TotkGuard::Report(trap);
    trap.words_read = false;
    TotkGuard::Report(trap);
    // Module names: the loader's list names the loop; a new process noting its modules again,
    // where the last one lay, starts a new list.
    int process = 0;
    Progress::NoteModule(&process, "rtld", 0x80000000, 0x6000);
    Progress::NoteModule(&process, "main", base, 0x3ADF000);
    Progress::NoteModule(&process, "subsdk0", 0x83E01000, 0x66C000);
    Progress::NoteModule(&process, "sdk", 0x8446D000, 0xC54000);
    char where[64];
    Progress::Where(where, sizeof(where), Progress::LoadedModules(&process), 0x80a03404);
    EXPECT(std::strcmp(where, "main+0x6e1404") == 0);
    Progress::Where(where, sizeof(where), Progress::LoadedModules(&process), 0x845e28f0);
    EXPECT(std::strcmp(where, "sdk+0x1758f0") == 0);
    int another = 0;
    EXPECT(Progress::LoadedModules(&another).count == 0);
    Progress::NoteModule(&process, "rtld", 0x80000000, 0x6000);
    Progress::NoteModule(&process, "main", 0x80b1a000, 0x3ADF000);
    EXPECT(Progress::LoadedModules(&process).count == 2);
    Progress::Where(where, sizeof(where), Progress::LoadedModules(&process), 0x811fb404);
    EXPECT(std::strcmp(where, "main+0x6e1404") == 0);
    std::fprintf(stdout, failed ? "TEST_FAILED\n" : "TEST_DONE\n");
    return failed;
}
'''

RUN = r'''
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstring>
extern "C" void run_original(void* src, void* dst, float bias, float scale, unsigned long* traps);
extern "C" void run_bounded(void* src, void* dst, float bias, float scale, unsigned long* traps);

int main() {
    static unsigned char src[0x5aa0 + 4 * 64], dst_a[0x2644 + 4 * 64], dst_b[0x2644 + 4 * 64];
    int failed = 0;
    for (std::uint32_t count : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 20u, 64u}) {
        std::memset(src, 0, sizeof(src));
        std::memset(dst_a, 0xEE, sizeof(dst_a));
        std::memset(dst_b, 0xEE, sizeof(dst_b));
        std::memcpy(src + 0x58e0, &count, 4);
        for (int i = 0; i < 64; ++i) {
            const float value = 1.5f * i - 3.0f;
            std::memcpy(src + 0x5aa0 + 4 * i, &value, 4);
        }
        unsigned long traps_a = 0, traps_b = 0;
        run_original(src, dst_a, 0.25f, 2.0f, &traps_a);
        run_bounded(src, dst_b, 0.25f, 2.0f, &traps_b);
        const std::uint32_t stored = count < 4 ? count : 4;
        // The bounded loop: the first min(count, 4) floats, exactly as the original stores them,
        // and nothing past them; one report when the count is above 4.
        if (std::memcmp(dst_a, dst_b, 0x2644 + 4 * stored) != 0) failed = 1;
        for (std::size_t at = 0x2644 + 4 * stored; at < sizeof(dst_b); ++at)
            if (dst_b[at] != 0xEE) failed = 1;
        if (traps_b != (count > 4 ? 1u : 0u) || traps_a != 0) failed = 1;
        // The original stores all `count` (past the 4 its destination holds, when above 4).
        for (std::uint32_t i = 0; i < count; ++i) {
            float value;
            std::memcpy(&value, dst_a + 0x2644 + 4 * i, 4);
            if (value != (1.5f * i - 3.0f) * 2.0f + 0.25f) failed = 1;
        }
        std::printf("count=%u original_stores=%u bounded_stores=%u reports=%lu\n", count, count, stored, traps_b);
    }
    std::printf(failed ? "RUN_FAILED\n" : "RUN_DONE\n");
    return failed;
}
'''


def run_loops(cxx, tmp):
    prefix = '_' if platform.system() == 'Darwin' else ''
    def function(name, body):
        insts = '\n'.join(f'    .inst {word:#010x}' for word in body)
        return f'''
    .globl {prefix}{name}
    .p2align 2
{prefix}{name}:
    stp x19, x20, [sp, #-16]!
    mov x19, x1
    mov x9, x0
    mov w10, #0x58e0
    add x10, x9, x10
    mov x14, #0
{insts}
    str x14, [x2]
    ldp x19, x20, [sp], #16
    ret
'''
    # Words 9 to 22 of the original window: from `ldr w11, [x10]` to `b.lo`, its cbz landing just
    # after; the bounded words with their system call counted in x14 instead of made.
    bounded = [0x910005ce if word == 0xd40003c1 else word for word in patched]
    (tmp / 'loops.S').write_text('.text\n' + function('run_original', original[9:23]) + function('run_bounded', bounded))
    (tmp / 'run.cpp').write_text(RUN)
    binary = tmp / 'run'
    built = subprocess.run([cxx, '-std=c++20', '-O1', str(tmp / 'run.cpp'), str(tmp / 'loops.S'), '-o', str(binary)],
                           capture_output=True, text=True)
    if built.returncode != 0:
        failures.append('the loops do not build:\n' + built.stderr[-3000:])
        return
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    print(result.stdout.strip())
    if result.returncode != 0 or 'RUN_DONE' not in result.stdout:
        failures.append('the loops ran differently than expected:\n' + result.stdout + result.stderr)


def check_sites(source):
    nso = (source / 'src/core/loader/nso.cpp').read_text()
    anchor = '    // Apply cheats if they exist and the program has a valid title ID\n'
    if nso.count(anchor) != 1:
        failures.append(f'nso.cpp: the cheat step found {nso.count(anchor)} times')
    else:
        load = nso[nso.index('std::optional<VAddr> AppLoader_NSO::LoadModule('):]
        if not (load.index('if (!load_into_process) {') < load.index(anchor) <
                load.index('process.LoadModule(system.Kernel(), std::move(codeset), load_base);')):
            failures.append('nso.cpp: the cheat step is no longer between the layout pass and the mapping')
    step = cmake[cmake.index('string(REPLACE "${old_nso_mapped}"'):]
    step = step[:step.index(']=]')]
    for needed in ('::Eden::Progress::NoteModule(&process, nso_file.GetName().c_str(), load_base + module_start, image_size);',
                   'nso_file.GetName() == "main"', '::Eden::TotkGuard::Apply(codeset.memory.data() + module_start,',
                   'nso_header.build_id.data(), load_base + module_start);', '::Eden::TotkGuard::ReportApplied('):
        if needed not in step:
            failures.append(f'CMakeLists.txt: the loader step lacks {needed}')
    write = cmake.index('write_derived("${PORT_BUILD_DIR}/nso.cpp" "${nso_source}")')
    if not (cmake.index('string(REPLACE "${old_nso_mapped}"') < write and
            'guest_progress.h' in cmake[cmake.index('string(REPLACE "${old_nso_mapped}"'):write] and
            'totk_guard.h' in cmake[cmake.index('string(REPLACE "${old_nso_mapped}"'):write]):
        failures.append('CMakeLists.txt: nso.cpp does not include the guard')
    # The report is made before the system call (which then goes on as usual), on every build.
    svc = re.search(r'string\(REPLACE "\$\{progress_svc_old\}"\s*"((?:[^"\\]|\\.)*)"', cmake).group(1)
    if not (svc.index('CoreSvc(') < svc.index('::Eden::Sample::TotkTrap(*process, *interface, *thread, interface->GetSvcNumber());')
            < svc.index('Svc::Call(system, interface->GetSvcNumber());')):
        failures.append('CMakeLists.txt: the bounded loop is not reported before its system call')
    core = (source / 'src/core/hle/kernel/physical_core.cpp').read_text()
    if core.count('            // Perform call.\n            Svc::Call(system, interface->GetSvcNumber());') != 1:
        failures.append('physical_core.cpp: the system call site changed')
    sample = (root / 'headless/guest_sample.h').read_text()
    trap = sample[sample.index('inline void TotkTrap('):]
    if 'GetPointerSilent' not in trap or 'Memory::Read' in trap:
        failures.append('guest_sample.h: the trap must read the words without a download from the GPU caches')


cxx = shutil.which('clang++-18') or shutil.which('clang++') or shutil.which('c++')
clang = shutil.which('clang-18') or shutil.which('clang')
with tempfile.TemporaryDirectory(prefix='eden-totk-') as tmp:
    tmp = Path(tmp)
    if clang:
        check_listings(clang, tmp)
    else:
        print('totk guard: no clang here; listings not compared')
    (tmp / 'test.cpp').write_text(TEST)
    built = subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-fsanitize=address,undefined', '-I' + str(root / 'headless'), str(tmp / 'test.cpp'),
                            '-o', str(tmp / 'test')], capture_output=True, text=True)
    if built.returncode != 0:
        failures.append('the test does not build:\n' + built.stderr[-3000:])
    else:
        result = subprocess.run([str(tmp / 'test')], capture_output=True, text=True, timeout=60)
        lines = result.stderr.splitlines()
        reports = [line for line in lines if line.startswith('EDEN_TOTK_LOOP_BOUNDED ')]
        if result.returncode != 0 or 'TEST_DONE' not in result.stdout:
            failures.append('the guard test failed:\n' + result.stdout + result.stderr[-2000:])
        # 41 reports made: lines for 1-8, 16 and 32.
        if [re.search(r'n=(\d+)', line).group(1) for line in reports] != ['1', '2', '3', '4', '5', '6', '7', '8', '16', '32']:
            failures.append(f'the report is not limited to the first 8 and powers of two: {reports}')
        expected = ('EDEN_TOTK_LOOP_BOUNDED n=1 count=940309209 (stored 4) count_at=0x217b5a08e0 page=gpu-cached '
                    'src=0x217b5a0aa0 dst=0x217b590be0 thread=79 words_from=0x217b5a08c0: 380bfad9 380bfada')
        if not reports or not reports[0].startswith(expected):
            failures.append(f'the report does not say what it found: {reports[:1]}')
        applied = [line for line in lines if line.startswith('[ProsperoEden] TOTK:')]
        if len(applied) != 2 or '(0x80a03404) stores at most 4 floats' not in applied[0] or 'left as it is' not in applied[1]:
            failures.append(f'the loader lines are not as expected: {applied}')
        print(f'totk guard: Apply, the report and module names pass ({len(reports)} of 41 reports printed)')
    if platform.machine().lower() in ('arm64', 'aarch64'):
        run_loops(cxx, tmp)
    else:
        print('totk guard: not an AArch64 host; the loops are not run here')

if len(sys.argv) > 1:
    check_sites(Path(sys.argv[1]))
if failures:
    sys.exit('totk guard check failed:\n  ' + '\n  '.join(failures))
print('TOTK guard: PASS the loop at main+0x6E1404 bounded to the 4 floats its destination holds, only on its '
      'build and words, a count above 4 reported, addresses named by module'
      + (', and every site in the pinned source' if len(sys.argv) > 1 else ''))
