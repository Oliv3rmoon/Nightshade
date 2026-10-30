// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the shader cache reader (shader_cache_reader.inc): a damaged file must neither
// stop the reading with an error nor lose the entries before the damage. The reader reads the
// file a piece at a time: each case runs with pieces from a few bytes (every entry, and every
// position the reader goes back to, crosses one) to the console's 8 MiB.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>
#include "shader_recompiler/stage.h"
#include "video_core/shader_environment.h"

namespace VideoCommon {
extern std::size_t shader_cache_chunk_bytes;
}

namespace {
constexpr std::uint32_t kVersion = 7;
constexpr std::size_t kHeader = 12, kKey = 8;

template <typename T>
void Put(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// One compute pipeline as Eden writes it: an environment with 16 bytes of code, then its key.
std::string Entry() {
    std::string out;
    Put<std::uint32_t>(out, 1);                    // environments
    Put<std::uint64_t>(out, 16);                   // code bytes
    for (int i = 0; i < 4; ++i) Put<std::uint64_t>(out, 0);  // texture types, formats, values, replacements
    for (int i = 0; i < 6; ++i) Put<std::uint32_t>(out, 0);  // local memory ... viewport transform
    Put(out, Shader::Stage::Compute);
    out.append(16, '\x11');                        // code
    out.append(16, '\0');                          // workgroup size, shared memory size
    out.append(kKey, '\x22');                      // key
    return out;
}

std::string Header() {
    std::string out{"yuzucach"};
    Put(out, kVersion);
    return out;
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s shader cache (read %zu bytes at a time): %s\n", ok ? "PASS" : "FAIL",
                VideoCommon::shader_cache_chunk_bytes, what);
    if (!ok) ++failures;
}

struct Loaded {
    int loaded;
    std::uintmax_t size;  // 0: the file is gone
};

Loaded Load(const std::filesystem::path& file, const std::string& content) {
    { std::ofstream(file, std::ios::binary) << content; }
    int loaded = 0;
    VideoCommon::LoadPipelines(
        std::stop_token{}, file, kVersion,
        [&](std::ifstream& in, VideoCommon::FileEnvironment) {
            std::array<char, kKey> key;
            in.read(key.data(), key.size());
            ++loaded;
        },
        [&](std::ifstream& in, std::vector<VideoCommon::FileEnvironment>) {
            std::array<char, kKey> key;
            in.read(key.data(), key.size());
            ++loaded;
        });
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    return {loaded, error ? 0 : size};
}
} // namespace

int main() {
    const auto file = std::filesystem::temp_directory_path() / "eden-shader-cache-check.bin";
    const std::string entry = Entry(), two = Header() + entry + entry;
    std::string many = Header();
    for (int i = 0; i < 300; ++i) many += entry;
    const std::size_t pieces[] = {1, 7, 13, 64, 4096, std::size_t{8} << 20};
    for (const std::size_t piece : pieces) {
        VideoCommon::shader_cache_chunk_bytes = piece;
        Loaded r = Load(file, two);
        Expect(r.loaded == 2 && r.size == two.size(), "a whole file is read and left as it is");

        r = Load(file, many);
        Expect(r.loaded == 300 && r.size == many.size(), "300 entries are all read");

        r = Load(file, many + entry.substr(0, entry.size() / 3));
        Expect(r.loaded == 300 && r.size == many.size(), "300 entries and one cut short: the 300 stay");

        r = Load(file, two + std::string(4096, '\0'));
        Expect(r.loaded == 2 && r.size == two.size(), "a zeroed tail is cut off, the entries before it stay");

        r = Load(file, two + entry.substr(0, entry.size() / 2));
        Expect(r.loaded == 2 && r.size == two.size(), "an entry cut short is cut off, the entries before it stay");

        std::string huge = entry;
        huge.replace(4, 8, std::string(8, '\x7f'));  // a code size far beyond the file
        r = Load(file, two + huge);
        Expect(r.loaded == 2 && r.size == two.size(), "an entry with an impossible size is cut off");

        r = Load(file, two + std::string(2048, '\xa5'));
        Expect(r.loaded == 2 && r.size == two.size(), "a random tail is cut off");

        r = Load(file, Header() + std::string(64, '\0'));
        Expect(r.loaded == 0 && r.size == kHeader, "a file with nothing whole keeps only its header");

        r = Load(file, std::string());
        Expect(r.loaded == 0 && r.size == 0, "an empty file is removed");

        r = Load(file, std::string(4096, '\0'));
        Expect(r.loaded == 0 && r.size == 0, "a zeroed file is removed");

        r = Load(file, two);
        Expect(r.loaded == 2 && r.size == two.size(), "and a whole file still reads after all that");
    }
    VideoCommon::shader_cache_chunk_bytes = std::size_t{8} << 20;

    std::filesystem::remove(file);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
