// SPDX-License-Identifier: GPL-3.0-or-later
// A game's start and its shader cache (the pipelines saved by earlier sessions, Eden's vulkan.bin).
//
// Where the time goes. Every build of a cached pipeline is timed by stage on the thread that
// builds it, and the driver says how it made the pipeline (VK_EXT_pipeline_creation_feedback):
// found in the pipeline cache Eden keeps for it (vulkan_pipelines.bin), or not. The heap says how
// many small requests its threads' arenas could not take (heap_arenas.inc). A finished build, or
// one stopped by leaving the game, leaves EDEN_SHADER_START lines in stderr.log
// (tools/prepare-vulkan-port.py writes the hooks into Eden's Vulkan pipeline cache).
//
// The build while the game plays. With background on (Settings, "background_shader_cache"), the
// game starts as soon as its graphics are ready, and the cache is read and built behind it: a
// pipeline the game needs before then is built on the GPU thread as any new one is (with
// asynchronous shaders the draws that use it are skipped until it is built). Loader is the state
// the reading thread, the threads that build what it reads and the GPU thread share for that, and
// what makes leaving the game in the middle of it safe.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" void eden_heap_spill_stats(unsigned long long* spills, unsigned long long* spill_cycles,
                                      unsigned long long* frees, unsigned long long* free_cycles)
    __attribute__((weak));
extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));

namespace Eden::ShaderStart {

// This session builds its shader cache while the game plays (main.cpp, from the settings).
inline std::atomic<bool> background{true};
// The device tells how each pipeline was made (pipeline creation feedback, core in Vulkan 1.3).
inline std::atomic<bool> feedback{false};

inline long long Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline unsigned long long Cycles() {
#if defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

// The stages of one cached pipeline's build, as the thread that builds it spends them.
enum Stage : unsigned {
    job,     // the whole build
    cfg,     // the guest shader's control flow (Maxwell)
    ir,      // Maxwell to Eden's IR, and its passes
    spirv,   // IR to SPIR-V
    shader_module,  // vkCreateShaderModule
    layout,  // descriptor set and pipeline layouts, update templates
    driver,  // vkCreateGraphicsPipelines / vkCreateComputePipelines
    stage_count
};
inline constexpr const char* kStageNames[stage_count]{"job", "cfg", "ir", "spirv", "module", "layout", "driver"};

struct Totals {
    std::atomic<unsigned long long> ns{0}, calls{0};
    void Add(long long elapsed) {
        ns.fetch_add(static_cast<unsigned long long>(elapsed > 0 ? elapsed : 0), std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    void Clear() {
        ns.store(0, std::memory_order_relaxed);
        calls.store(0, std::memory_order_relaxed);
    }
};
inline std::array<Totals, stage_count> stages{};

// How the driver made each pipeline: found in the pipeline cache Eden gave it (the driver's own
// verdict); not found there, by the driver's own time for it (under 2 ms: from its disk cache,
// most likely; longer: compiled); or the driver did not say.
enum Driver : unsigned { app_cache, other_quick, other_slow, unsaid, driver_count };
inline constexpr const char* kDriverNames[driver_count]{"app_cache", "elsewhere_under_2ms", "compiled", "unsaid"};
inline std::array<Totals, driver_count> drivers{};                  // the calls' wall time
inline std::atomic<unsigned long long> driver_own_ns[driver_count]{};  // the driver's own time

// A cached pipeline is being built on this thread (Job): the stages below count only then, so a
// pipeline a game makes while it plays is not counted, nor slowed.
inline thread_local bool counting = false;

class Clock {
public:
    explicit Clock(Stage which) : stage{which}, start{counting ? Now() : 0} {}
    ~Clock() {
        Stop();
    }
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    void Stop() {
        if (start != 0) stages[stage].Add(Now() - start);
        start = 0;
    }

private:
    Stage stage;
    long long start;
};

class Job {
public:
    Job() : previous{counting}, start{Now()} {
        counting = true;
    }
    ~Job() {
        counting = previous;
        stages[job].Add(Now() - start);
    }
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

private:
    bool previous;
    long long start;
};

// One vkCreate*Pipelines call made for the cache: `valid` is the feedback's valid bit.
inline void NoteDriver(long long wall_ns, bool valid, bool application_cache_hit, unsigned long long driver_ns) {
    stages[driver].Add(wall_ns);
    const Driver kind = !valid                       ? unsaid
                        : application_cache_hit      ? app_cache
                        : driver_ns < 2'000'000ULL   ? other_quick
                                                     : other_slow;
    drivers[kind].Add(wall_ns);
    driver_own_ns[kind].fetch_add(valid ? driver_ns : 0, std::memory_order_relaxed);
}

// The heap's spills at a moment, and the cycle counter's rate between two moments.
struct HeapMark {
    unsigned long long spills = 0, spill_cycles = 0, frees = 0, free_cycles = 0, cycles = 0;
    long long ns = 0;
    unsigned arenas = 0;
    static HeapMark Take() {
        HeapMark mark;
        if (eden_heap_spill_stats) eden_heap_spill_stats(&mark.spills, &mark.spill_cycles, &mark.frees, &mark.free_cycles);
        mark.arenas = eden_heap_arenas_created ? eden_heap_arenas_created() : 0;
        mark.cycles = Cycles();
        mark.ns = Now();
        return mark;
    }
};

inline void Reset() {
    for (auto& stage : stages) stage.Clear();
    for (auto& kind : drivers) kind.Clear();
    for (auto& own : driver_own_ns) own.store(0, std::memory_order_relaxed);
}

// What the vk side knows of one load, for the report.
struct Summary {
    bool background = false;
    bool stopped = false;
    std::size_t pipelines = 0, graphics = 0, compute = 0;  // read from the file and built
    std::size_t kept = 0;          // built and kept for the game
    std::size_t gpu_built = 0;     // the game needed before they were built: built by the GPU thread
    std::size_t taken = 0;         // built here and taken by the game while the cache was still loading
    std::size_t workers = 0;
    unsigned long long file_bytes = 0, driver_cache_bytes = 0;
    long long wall_ns = 0, read_ns = 0, read_wait_ns = 0, driver_cache_load_ns = 0, save_ns = 0;
};

inline double Ms(unsigned long long ns) {
    return static_cast<double>(ns) / 1e6;
}

// The EDEN_SHADER_START lines (stderr.log).
inline void Report(std::FILE* out, const Summary& s, const HeapMark& heap_start) {
    const HeapMark heap_end = HeapMark::Take();
    std::fprintf(out,
                 "EDEN_SHADER_START mode=%s stopped=%d pipelines=%zu graphics=%zu compute=%zu kept=%zu "
                 "gpu_thread_built=%zu taken_while_loading=%zu workers=%zu wall_ms=%.0f read_ms=%.0f "
                 "read_wait_ms=%.0f file_mb=%.1f driver_cache_mb=%.1f driver_cache_load_ms=%.0f save_ms=%.0f\n",
                 s.background ? "background" : "before_start", int(s.stopped), s.pipelines, s.graphics, s.compute,
                 s.kept, s.gpu_built, s.taken, s.workers, Ms(s.wall_ns), Ms(s.read_ns), Ms(s.read_wait_ns),
                 static_cast<double>(s.file_bytes) / 1048576.0, static_cast<double>(s.driver_cache_bytes) / 1048576.0,
                 Ms(s.driver_cache_load_ns), Ms(s.save_ns));
    // Summed over the threads that built: job is the whole of each build, the rest its parts, and
    // other what the parts leave out of job (pools, environments, waits for locks and memory).
    const unsigned long long job_ns = stages[job].ns.load(std::memory_order_relaxed);
    unsigned long long parts = 0;
    char line[512];
    int at = std::snprintf(line, sizeof(line), "EDEN_SHADER_START_STAGES");
    for (unsigned stage = 0; stage < stage_count; ++stage) {
        const unsigned long long ns = stages[stage].ns.load(std::memory_order_relaxed);
        if (stage != job) parts += ns;
        at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), " %s_ms=%.0f", kStageNames[stage],
                            Ms(ns));
    }
    const double busy = s.wall_ns > 0 ? static_cast<double>(job_ns) / static_cast<double>(s.wall_ns) : 0.0;
    std::fprintf(out, "%s other_ms=%.0f jobs=%llu threads_busy=%.1f\n", line,
                 Ms(job_ns > parts ? job_ns - parts : 0), stages[job].calls.load(std::memory_order_relaxed), busy);
    at = std::snprintf(line, sizeof(line), "EDEN_SHADER_START_DRIVER feedback=%d",
                       int(feedback.load(std::memory_order_relaxed)));
    for (unsigned kind = 0; kind < driver_count; ++kind) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), " %s=%llu %s_ms=%.0f %s_own_ms=%.0f",
                            kDriverNames[kind], drivers[kind].calls.load(std::memory_order_relaxed), kDriverNames[kind],
                            Ms(drivers[kind].ns.load(std::memory_order_relaxed)), kDriverNames[kind],
                            Ms(driver_own_ns[kind].load(std::memory_order_relaxed)));
    }
    std::fprintf(out, "%s\n", line);
    // The heap: small requests that went to the spaces every thread shares (and their lock), and
    // frees there, with the time they took, from the cycle counter's rate over the load.
    const double cycles_per_ns = heap_end.ns > heap_start.ns && heap_end.cycles > heap_start.cycles
                                     ? static_cast<double>(heap_end.cycles - heap_start.cycles) /
                                           static_cast<double>(heap_end.ns - heap_start.ns)
                                     : 0.0;
    const auto ms = [&](unsigned long long cycles) {
        return cycles_per_ns > 0 ? static_cast<double>(cycles) / cycles_per_ns / 1e6 : 0.0;
    };
    std::fprintf(out,
                 "EDEN_SHADER_START_HEAP spills=%llu spill_ms=%.0f shared_frees=%llu shared_free_ms=%.0f "
                 "arenas=%u arenas_made=%u\n",
                 heap_end.spills - heap_start.spills, ms(heap_end.spill_cycles - heap_start.spill_cycles),
                 heap_end.frees - heap_start.frees, ms(heap_end.free_cycles - heap_start.free_cycles), heap_end.arenas,
                 heap_end.arenas - heap_start.arenas);
    std::fflush(out);
}

// Progress for the loading screen and the log: built, and an estimate of the total while the
// file is still being read (what was read so far, scaled by how much of the file that was).
struct Progress {
    bool active = false, reading = false;
    std::size_t built = 0, total = 0;
};
inline std::mutex progress_mutex;
inline Progress progress_now;
inline void SetProgress(const Progress& value) {
    std::scoped_lock lock{progress_mutex};
    progress_now = value;
}
inline Progress GetProgress() {
    std::scoped_lock lock{progress_mutex};
    return progress_now;
}

// The state of one load, shared by the reader, the builders and the GPU thread (see the top).
// Key types have std::hash; `hash` arguments are the keys' own 64-bit hashes (Eden's Hash()),
// used where a rare collision costs only work: a pipeline built twice, or one not saved.
template <class GraphicsKey, class Graphics, class ComputeKey, class Compute, class Task>
class Loader {
public:
    using GraphicsPtr = std::unique_ptr<Graphics>;
    using ComputePtr = std::unique_ptr<Compute>;

    // At most `most_in_flight` entries read and not yet built: the file is read as the builders
    // need it, not ahead of them (all of it was queued at once: the heap grew by 1.9 GB during
    // Tears of the Kingdom's start, and keeps what it grows by), and a pipeline the GPU
    // thread asks the builders for waits behind that many at most.
    Loader(std::size_t most_in_flight, std::uint64_t file_bytes)
        : most{std::max<std::size_t>(most_in_flight, 1)}, file_size{file_bytes} {}

    // ---- the reader ----

    // One entry read from the file (its key's hash, and the file position after it), built or not.
    void Note(std::uint64_t hash, bool compute, std::uint64_t position) {
        std::scoped_lock lock{mutex};
        (compute ? file_compute : file_graphics).insert(hash);
        read_position = position;
    }

    // The entry just noted is to be built: waits for room among the builds in flight. False when
    // the load is stopping, and then it is not built.
    bool Admit(bool compute, long long* waited_ns) {
        std::unique_lock lock{mutex};
        const long long start = Now();
        const bool room = changed.wait(lock, stop.get_token(), [this] { return in_flight < most; });
        if (waited_ns) *waited_ns += Now() - start;
        if (!room || stop.stop_requested()) return false;
        ++in_flight;
        ++(compute ? admitted_compute : admitted_graphics);
        PublishLocked();
        return true;
    }

    // The reading ended: `whole` when the file was read to its end (a damaged tail cut off). The
    // pipelines the GPU thread made meanwhile could not be told apart from the file's: returns
    // the saves of those that are not in it (none when the load stopped).
    std::vector<Task> FinishReading(bool whole) {
        std::vector<Task> to_write;
        std::vector<Parked> dropped;
        {
            std::scoped_lock lock{mutex};
            reading = false;
            complete = whole && !stop.stop_requested();
            for (Parked& entry : parked) {
                if (complete && !InFileLocked(entry.hash, entry.compute)) to_write.push_back(std::move(entry.job));
                else dropped.push_back(std::move(entry));
            }
            parked.clear();
            PublishLocked();
            changed.notify_all();
        }
        return to_write;  // `dropped` goes here, outside the lock
    }

    // Waits until every admitted build has ended, calling `tick` every `period`; false when the
    // load stopped first.
    template <class Tick>
    bool WaitBuilt(std::chrono::milliseconds period, Tick&& tick) {
        std::unique_lock lock{mutex};
        while (in_flight != 0 && !stop.stop_requested()) {
            if (!changed.wait_for(lock, stop.get_token(), period, [this] { return in_flight == 0; })) {
                lock.unlock();
                tick();
                lock.lock();
            }
        }
        return !stop.stop_requested();
    }

    // ---- the builders ----

    // Whether to build an entry: not when the load is stopping, nor when the GPU thread built it.
    bool Wanted(std::uint64_t hash, bool compute) const {
        std::scoped_lock lock{mutex};
        return !stop.stop_requested() && !(compute ? claimed_compute : claimed_graphics).contains(hash);
    }

    // A pipeline built: kept for the GPU thread, unless the GPU thread built it meanwhile, the file
    // held it twice or the load is stopping. What is not kept comes back, for the caller to
    // destroy outside the lock.
    GraphicsPtr OfferGraphics(const GraphicsKey& key, std::uint64_t hash, GraphicsPtr pipeline) {
        std::scoped_lock lock{mutex};
        if (!pipeline || stop.stop_requested() || claimed_graphics.contains(hash) || graphics.contains(key))
            return pipeline;
        graphics.emplace(key, std::move(pipeline));
        ++kept;
        return nullptr;
    }
    ComputePtr OfferCompute(const ComputeKey& key, std::uint64_t hash, ComputePtr pipeline) {
        std::scoped_lock lock{mutex};
        if (!pipeline || stop.stop_requested() || claimed_compute.contains(hash) || compute.contains(key))
            return pipeline;
        compute.emplace(key, std::move(pipeline));
        ++kept;
        return nullptr;
    }

    // One admitted entry ended, built or not.
    void Done() {
        std::scoped_lock lock{mutex};
        --in_flight;
        ++finished;
        PublishLocked();
        changed.notify_all();
    }

    // ---- the GPU thread ----

    // A pipeline the game asks for that is not among its own: the one built here, or none, and
    // then (while the load goes on) the builders leave it to the GPU thread.
    GraphicsPtr TakeGraphics(const GraphicsKey& key, std::uint64_t hash) {
        std::scoped_lock lock{mutex};
        if (const auto found = graphics.find(key); found != graphics.end()) {
            GraphicsPtr pipeline = std::move(found->second);
            graphics.erase(found);
            if (!FinishedLocked()) {
                // A second entry of the file for it, built later, is not kept.
                claimed_graphics.insert(hash);
                ++taken;
            }
            return pipeline;
        }
        if (!FinishedLocked() && claimed_graphics.insert(hash).second) ++gpu_built;
        return nullptr;
    }
    ComputePtr TakeCompute(const ComputeKey& key, std::uint64_t hash) {
        std::scoped_lock lock{mutex};
        if (const auto found = compute.find(key); found != compute.end()) {
            ComputePtr pipeline = std::move(found->second);
            compute.erase(found);
            if (!FinishedLocked()) {
                // A second entry of the file for it, built later, is not kept.
                claimed_compute.insert(hash);
                ++taken;
            }
            return pipeline;
        }
        if (!FinishedLocked() && claimed_compute.insert(hash).second) ++gpu_built;
        return nullptr;
    }

    // The save of a pipeline the GPU thread made: returned to be written now, or kept until the
    // reading ends (until then it is not known whether the file has it), or dropped when the file
    // has it.
    std::optional<Task> Save(std::uint64_t hash, bool compute, Task job) {
        std::scoped_lock lock{mutex};
        if (reading) {
            parked.push_back(Parked{hash, compute, std::move(job)});
            return std::nullopt;
        }
        if (complete && InFileLocked(hash, compute)) return std::nullopt;
        return std::optional<Task>{std::move(job)};
    }

    // ---- the end ----

    // Foreground: the progress callback, called under the lock as builds end (built, total).
    void SetCallback(std::function<void(std::size_t, std::size_t)> callback) {
        std::scoped_lock lock{mutex};
        progress = std::move(callback);
    }

    std::stop_token StopToken() const {
        return stop.get_token();
    }
    bool Stopping() const {
        return stop.stop_requested();
    }
    void Stop() {
        stop.request_stop();
        std::scoped_lock lock{mutex};
        changed.notify_all();
    }

    // Until no admitted entry is in flight (after Stop, the ones left end without building).
    void WaitIdle() {
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] { return in_flight == 0; });
    }

    // Everything kept, handed over (and the load's state cleared).
    std::pair<std::unordered_map<GraphicsKey, GraphicsPtr>, std::unordered_map<ComputeKey, ComputePtr>> TakeAll() {
        std::scoped_lock lock{mutex};
        auto result = std::make_pair(std::move(graphics), std::move(compute));
        graphics.clear();
        compute.clear();
        return result;
    }

    struct Counts {
        std::size_t graphics = 0, compute = 0, finished = 0, kept = 0, gpu_built = 0, taken = 0, waiting = 0;
        bool reading = false, complete = false;
    };
    Counts GetCounts() const {
        std::scoped_lock lock{mutex};
        return Counts{admitted_graphics, admitted_compute, finished, kept, gpu_built, taken,
                      graphics.size() + compute.size(), reading, complete};
    }

private:
    struct Parked {
        std::uint64_t hash;
        bool compute;
        Task job;
    };

    bool InFileLocked(std::uint64_t hash, bool is_compute) const {
        return (is_compute ? file_compute : file_graphics).contains(hash);
    }
    bool FinishedLocked() const {
        return !reading && in_flight == 0;
    }
    std::size_t EstimateLocked() const {
        const std::size_t admitted = admitted_graphics + admitted_compute;
        if (!reading || read_position == 0 || file_size <= read_position) return admitted;
        return static_cast<std::size_t>(static_cast<double>(admitted) * static_cast<double>(file_size) /
                                        static_cast<double>(read_position));
    }
    void PublishLocked() {
        const std::size_t total = EstimateLocked();
        SetProgress(Progress{true, reading, finished, total});
        if (progress) progress(finished, total);
    }

    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::stop_source stop;
    const std::size_t most;
    const std::uint64_t file_size;
    std::uint64_t read_position = 0;
    std::size_t in_flight = 0, admitted_graphics = 0, admitted_compute = 0, finished = 0, kept = 0;
    std::size_t gpu_built = 0, taken = 0;
    bool reading = true, complete = false;
    std::unordered_set<std::uint64_t> file_graphics, file_compute, claimed_graphics, claimed_compute;
    std::unordered_map<GraphicsKey, GraphicsPtr> graphics;
    std::unordered_map<ComputeKey, ComputePtr> compute;
    std::vector<Parked> parked;
    std::function<void(std::size_t, std::size_t)> progress;
};

} // namespace Eden::ShaderStart
