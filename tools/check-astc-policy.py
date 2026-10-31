#!/usr/bin/env python3
"""Host check of where an ASTC texture kept as BC3 is converted (headless/astc_policy.h).

1. The policy: in place on light screens, in the background from the first busy frame and back;
   on a light frame, a texture larger than a screen picture goes to the background too (a light
   game in play: Top Racer Collection, Oliv3rmoon/Nightshade#3), unless it is larger than
   kLightBackgroundTexels, which stays in place.
2. With Eden's source as the argument, the Vulkan texture cache as tools/prepare-vulkan-port.py
   writes it: the code it puts in RefreshContents, compiled as written with stand-ins for the
   texture cache, sends each size where the policy says and logs EDEN_ASTC_BACKGROUND for a light
   frame's background conversion and EDEN_ASTC_IN_PLACE for one in place that held the frame for
   16 ms or more, and nothing for a texture that is not ASTC.
"""
from pathlib import Path
import subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
code = r"""
#include "astc_policy.h"
#include <cassert>
using namespace Eden::AstcPolicy;
static void Play(unsigned draws) { for (unsigned i = 0; i < draws; ++i) Draw(); }
int main() {
    constexpr unsigned picture = 1920 * 1080, mipmapped = picture + picture / 3, atlas = 4096 * 4096;
    constexpr unsigned array = 121 * (1024 * 1024 + (1024 * 1024) / 3);  // a large game's texture array
    // Nothing drawn yet, and a loading screen: a picture in place, a larger texture in the background.
    assert(!Busy() && !InBackground(picture) && !InBackground(mipmapped) && !InBackground(kInPlaceTexels));
    assert(InBackground(kInPlaceTexels + 1) && InBackground(atlas) && InBackground(kLightBackgroundTexels));
    // Above kLightBackgroundTexels a light frame converts in place, a layer at a time.
    assert(!InBackground(kLightBackgroundTexels + 1) && !InBackground(array));
    Play(400); assert(!InBackground(picture) && InBackground(atlas));
    Frame(); assert(!InBackground(picture));
    Play(700); Frame(); assert(!InBackground(picture) && Draws() == 700);
    // The first frame of play counts as busy once it has its draws, and the frames after it:
    // every size in the background.
    Play(kBusyFrame - 1); assert(!InBackground(picture));
    Draw(); assert(Busy() && InBackground(picture) && InBackground(array));
    Play(2500); Frame(); assert(InBackground(picture));
    Play(3000); Frame(); assert(InBackground(1));
    // A menu opens: its first frame still follows a busy one, the next does not.
    Play(300); assert(InBackground(picture));
    Frame(); Play(300); assert(!InBackground(picture) && InBackground(atlas));
    Frame(); assert(!InBackground(picture));
    // The log: in place from 16 ms, the first 32 and then every 64th; the light background's
    // first 16 and then every 64th.
    assert(!ReportSlowInPlace(kSlowInPlaceNs - 1) && slow_in_place == 0);
    for (unsigned n = 1; n <= 32; ++n) assert(ReportSlowInPlace(kSlowInPlaceNs));
    for (unsigned n = 33; n < 64; ++n) assert(!ReportSlowInPlace(kSlowInPlaceNs));
    assert(ReportSlowInPlace(kSlowInPlaceNs) && slow_in_place == 64 && slow_in_place_ns == 64 * kSlowInPlaceNs);
    for (unsigned n = 1; n <= 16; ++n) assert(ReportLightBackground());
    for (unsigned n = 17; n < 64; ++n) assert(!ReportLightBackground());
    assert(ReportLightBackground());
    return 0;
}
"""
# The code tools/prepare-vulkan-port.py puts in TextureCache::RefreshContents, between the stand-ins.
cache_code = r"""
#include <cassert>
#include <cstdio>
#include "astc_policy.h"
#include "memory_budget.h"
#include "common/scope_exit.h"
namespace Eden::Performance {
long long now = 1;
long long NowNs() { return now; }
}
enum class ImageFlagBits : unsigned { None = 0, AsynchronousDecode = 1u << 16 };
constexpr ImageFlagBits operator&(ImageFlagBits a, ImageFlagBits b) {
    return static_cast<ImageFlagBits>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}
constexpr bool True(ImageFlagBits f) { return static_cast<unsigned>(f) != 0; }
struct Extent { unsigned width, height; };
struct Resources { int levels, layers; };
struct Info { Extent size; Resources resources; };
struct Image { ImageFlagBits flags; Info info; unsigned texels; };
using ImageId = int;
unsigned MapSizeBytes(const Image& image) { return image.texels; }
int queued = 0, in_place = 0;
void QueueAsyncDecode(Image&, ImageId) { ++queued; }
void RefreshContents(Image& image, ImageId image_id, long long conversion_ns) {
@SNIPPET@
    Eden::Performance::now += conversion_ns;  // UploadImageContents
    ++in_place;
}
int main() {
    using namespace Eden::AstcPolicy;
    const auto astc = [](unsigned width, unsigned height, int layers, int levels) {
        return Image{ImageFlagBits::AsynchronousDecode, {{width, height}, {levels, layers}}, width * height * unsigned(layers)};
    };
    Image picture = astc(1920, 1080, 1, 1), atlas = astc(4096, 4096, 1, 1), array = astc(1024, 1024, 64, 1);
    Image other{ImageFlagBits::None, {{4096, 4096}, {1, 1}}, 4096u * 4096u};
    for (int i = 0; i < 200; ++i) Draw();  // a light game in play
    RefreshContents(picture, 1, 20'000'000);  // in place, 20 ms: logged
    assert(in_place == 1 && queued == 0);
    RefreshContents(picture, 1, 5'000'000);   // in place, 5 ms: not logged
    RefreshContents(atlas, 2, 0);             // to the background, logged
    assert(in_place == 2 && queued == 1);
    RefreshContents(array, 3, 700'000'000);   // in place: too large for the background
    assert(in_place == 3 && queued == 1);
    RefreshContents(other, 4, 900'000'000);   // not ASTC: neither moved nor logged
    assert(in_place == 4 && queued == 1);
    for (int i = 0; i < 2000; ++i) Draw();    // a busy frame: everything in the background, unlogged
    RefreshContents(picture, 1, 0);
    RefreshContents(array, 3, 0);
    assert(in_place == 4 && queued == 3);
    // A light frame again, with 120 MiB of conversions in flight: the next goes in place
    // (memory_budget.h LightBackgroundRoom), unlogged at 0 ms.
    Frame();
    Frame();
    Eden::Performance::astc_background_bytes = 120ull << 20;
    RefreshContents(atlas, 2, 0);
    assert(in_place == 5 && queued == 3);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'policy'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
    print('ASTC conversion: PASS in place on light screens up to a picture, larger ones in the background unless '
          'over 32 Mi texels, everything in the background from the first busy frame, and back')
    if len(sys.argv) > 1:
        source = Path(sys.argv[1])
        port = Path(tmp) / 'port'
        port.mkdir()
        subprocess.run([sys.executable, '-B', str(root / 'tools/prepare-vulkan-port.py'), str(source), str(port)],
                       check=True, stdout=subprocess.DEVNULL)
        cache = (port / 'vulkan-cache/video_core/texture_cache/texture_cache.h').read_text()
        refresh = cache.split('void TextureCache<P>::RefreshContents(Image& image, ImageId image_id) {', 1)[1]
        refresh = refresh.split('\ntemplate <class P>', 1)[0]
        begin = refresh.index('    if (True(image.flags & ImageFlagBits::AsynchronousDecode) && '
                              '::Eden::AstcPolicy::InBackground(MapSizeBytes(image))')
        end = refresh.index('    };\n', begin) + len('    };\n')
        snippet = refresh[begin:end]
        # The conversion in place it times is the upload that follows it.
        assert refresh.index('UploadImageContents(image, staging);', end) > end
        assert 'QueueAsyncDecode(image, image_id);' not in refresh[:begin]
        exe = Path(tmp) / 'cache'
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I' + str(root / 'headless'), '-I' + str(source / 'src'), '-x', 'c++', '-', '-o', str(exe)],
                       input=cache_code.replace('@SNIPPET@', snippet), text=True, check=True)
        lines = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
        expected = [
            'EDEN_ASTC_IN_PLACE ms=20.0 texels=2073600 size=1920x1080 layers=1 levels=1 draws=200 slow=1 slow_ms=20',
            'EDEN_ASTC_BACKGROUND texels=16777216 size=4096x4096 layers=1 levels=1 draws=200 count=1',
            'EDEN_ASTC_IN_PLACE ms=700.0 texels=67108864 size=1024x1024 layers=64 levels=1 draws=200 slow=2 slow_ms=720',
        ]
        assert lines == expected, lines
        print('ASTC conversion: PASS the Vulkan texture cache\'s code as written sends each size where the policy '
              'says, logs a light frame\'s background conversion and a slow one in place, and leaves other textures')
