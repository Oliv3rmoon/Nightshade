// SPDX-License-Identifier: GPL-3.0-or-later
// Reading a game's shader cache file (in place of Eden's LoadPipelines; headless/CMakeLists.txt).
// The file grows by one entry per new pipeline while a game runs, so a crash or a console that
// goes down can leave it with a tail that is cut off, zeroed or random. Eden removed a file that
// ended early and trusted everything else: an entry announcing no shaders, or sizes far beyond
// the file, made every later start of that game fail the same way. Here an entry is checked
// before it is read, any failure ends the reading, and the file is cut back to its last whole
// entry: those stay usable and the game builds the rest again as it runs.
//
// The file is read in pieces of shader_cache_chunk_bytes. Through the C++ library's file stream
// every 4 KiB was a system call of its own, and every position asked of the stream (each entry
// and each of its shaders: tellg, seekg) threw its buffer away and asked the file again. A system
// call on the console's storage costs about the same whatever its size, and reading Tears of the
// Kingdom's 16,000 pipelines took 15 s. Positions here are counted in the buffer, and a piece is
// read only when the reading reaches it.
// (Included inside namespace VideoCommon: the headers it needs are added at the top of the file,
// headless/CMakeLists.txt.)

// How much of the file one read takes (shader_cache_check.cpp makes it small to cross pieces).
std::size_t shader_cache_chunk_bytes = std::size_t{8} << 20;

namespace {
struct DamagedCache {};

// The file, a piece at a time, for an istream: reads, and positions within what was read, cost no
// system call; a position outside it makes the next read start there.
class CacheFileBuffer final : public std::streambuf {
public:
    explicit CacheFileBuffer(std::size_t chunk) : buffer(chunk == 0 ? 1 : chunk) {}
    ~CacheFileBuffer() override {
        Close();
    }
    CacheFileBuffer(const CacheFileBuffer&) = delete;
    CacheFileBuffer& operator=(const CacheFileBuffer&) = delete;

    bool Open(const std::filesystem::path& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        struct stat info {};
        if (::fstat(fd, &info) != 0) {
            Close();
            return false;
        }
        size = static_cast<std::streamoff>(info.st_size);
        return true;
    }
    void Close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    std::streamoff Size() const {
        return size;
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        start += egptr() - eback();  // the piece in the buffer was read through
        char* const data = buffer.data();
        setg(data, data, data);
        if (fd < 0 || start >= size) {
            return traits_type::eof();
        }
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::streamoff>(static_cast<std::streamoff>(buffer.size()), size - start));
        std::size_t got = 0;
        while (got < want) {
            const ssize_t read = ::pread(fd, data + got, want - got, static_cast<off_t>(start + got));
            if (read < 0 && errno == EINTR) {
                continue;
            }
            if (read <= 0) {
                break;
            }
            got += static_cast<std::size_t>(read);
        }
        setg(data, data, data + got);
        return got == 0 ? traits_type::eof() : traits_type::to_int_type(*gptr());
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override {
        const off_type here = start + (gptr() - eback());
        const off_type target = direction == std::ios_base::beg   ? offset
                                : direction == std::ios_base::cur ? here + offset
                                                                  : size + offset;
        return seekpos(pos_type(target), which);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
        const off_type target = off_type(position);
        if (!(which & std::ios_base::in) || target < 0 || target > size) {
            return pos_type(off_type(-1));
        }
        if (target >= start && target <= start + (egptr() - eback())) {
            setg(eback(), eback() + (target - start), egptr());
        } else {
            start = target;
            char* const data = buffer.data();
            setg(data, data, data);
        }
        return position;
    }

private:
    std::vector<char> buffer;
    int fd = -1;
    std::streamoff size = 0;
    std::streamoff start = 0;  // where in the file the buffer's first byte is
};

// An entry's environment begins with five sizes; none can exceed what is left of the file.
void CheckEnvironment(std::ifstream& file, std::streamoff end) {
    const std::streamoff start = file.tellg();
    std::array<u64, 5> sizes{};
    file.read(reinterpret_cast<char*>(sizes.data()), sizeof(sizes));
    const u64 left = static_cast<u64>(end - start);
    for (const u64 size : sizes) {
        if (size > left) throw DamagedCache{};
    }
    file.seekg(start);
}
} // namespace

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics) {
    // A pipeline has at most this many shaders (two vertex programs, tessellation control and
    // evaluation, geometry, fragment).
    constexpr u32 kMostShaders = 6;
    std::streamoff good = -1;  // the end of the last whole entry; -1: not even the header
    try {
        CacheFileBuffer reader(shader_cache_chunk_bytes);
        if (!reader.Open(filename)) {
            return;
        }
        // The callers read from an std::ifstream: this one reads through the buffer above (its own
        // file buffer is never opened, and the stream is never closed through it).
        std::ifstream file;
        static_cast<std::ios&>(file).rdbuf(&reader);
        file.exceptions(std::ifstream::failbit);
        const std::streamoff end{reader.Size()};

        std::array<char, 8> magic_number;
        u32 cache_version;
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
            reader.Close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return;
        }
        good = file.tellg();
        while (good != end) {
            if (stop_loading.stop_requested()) {
                return;
            }
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            if (num_envs == 0 || num_envs > kMostShaders) {
                throw DamagedCache{};
            }
            std::vector<FileEnvironment> envs(num_envs);
            for (FileEnvironment& env : envs) {
                CheckEnvironment(file, end);
                env.Deserialize(file);
            }
            if (envs.front().ShaderStage() == Shader::Stage::Compute) {
                load_compute(file, std::move(envs.front()));
            } else {
                load_graphics(file, std::move(envs));
            }
            good = file.tellg();
        }
        return;
    } catch (const std::ios_base::failure&) {
    } catch (const DamagedCache&) {
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
    std::error_code error;
    if (good > 0) {
        std::filesystem::resize_file(filename, static_cast<std::uintmax_t>(good), error);
    }
    if (good <= 0 || error) {
        (void)Common::FS::RemoveFile(filename);
    }
    std::fprintf(stderr, "[ProsperoEden] shader cache: %s is damaged after %lld bytes; %s\n",
                 Common::FS::PathToUTF8String(filename).c_str(), static_cast<long long>(good < 0 ? 0 : good),
                 good > 0 && !error ? "the entries before that are kept" : "the file is removed");
}
