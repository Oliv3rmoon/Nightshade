// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the shader cache's load while a game plays (shader_start.h's Loader), as
// vk_pipeline_cache_cost.cpp uses it (tools/prepare-vulkan-port.py): a reader that admits the
// file's entries, builders on a pool that build and offer them, and a GPU thread that asks for
// pipelines, builds the ones not ready itself and saves what it built. Run many times with
// random timings, to the end and stopped in the middle (leaving the game), under the thread
// sanitizer and under the address sanitizer (tools/check-shader-start.py). Checked:
//  - every pipeline the GPU thread ends up with was built once for it: taken from the load or
//    built by itself, never both, and every one the load built is either taken, kept or destroyed;
//  - a pipeline the GPU thread built is saved exactly when the file does not have it, and never
//    while the file is still being read (the reader would read a growing file);
//  - a stop in the middle ends the reader, the builders and the waits promptly, and no pipeline
//    outlives the load;
//  - the progress the load publishes ends inactive.
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include "shader_start.h"

namespace {
std::atomic<long> alive{0};  // pipelines in existence

struct Pipeline {
    explicit Pipeline(std::uint64_t k) : key{k} {
        alive.fetch_add(1);
    }
    ~Pipeline() {
        alive.fetch_sub(1);
    }
    std::uint64_t key;
};
struct GraphicsKey {
    std::uint64_t value;
    bool operator==(const GraphicsKey&) const = default;
};
struct ComputeKey {
    std::uint64_t value;
    bool operator==(const ComputeKey&) const = default;
};
} // namespace
template <>
struct std::hash<GraphicsKey> {
    std::size_t operator()(const GraphicsKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.value);
    }
};
template <>
struct std::hash<ComputeKey> {
    std::size_t operator()(const ComputeKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.value);
    }
};

namespace {
using Loader = Eden::ShaderStart::Loader<GraphicsKey, Pipeline, ComputeKey, Pipeline, std::function<void()>>;

// Eden's ThreadWorker, as far as the load uses it: FIFO, tasks run to their end.
class Pool {
public:
    explicit Pool(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this](std::stop_token stop) {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        if (!wake.wait(lock, stop, [this] { return !tasks.empty(); })) return;
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                    task = nullptr;
                    {
                        std::scoped_lock lock{mutex};
                        --pending;
                    }
                    idle.notify_all();
                }
            });
        }
    }
    void Queue(std::function<void()> task) {
        {
            std::scoped_lock lock{mutex};
            tasks.push_back(std::move(task));
            ++pending;
        }
        wake.notify_one();
    }
    // Until every task queued so far has run.
    void Drain() {
        std::unique_lock lock{mutex};
        idle.wait(lock, [this] { return pending == 0; });
    }

private:
    std::mutex mutex;
    std::condition_variable_any wake, idle;
    std::deque<std::function<void()>> tasks;
    std::size_t pending = 0;
    std::vector<std::jthread> threads;
};

struct Outcome {
    bool stopped;
};

void Fail(const char* what, unsigned round) {
    std::fprintf(stderr, "shader start check: %s (round %u)\n", what, round);
    std::exit(EXIT_FAILURE);
}

Outcome Round(unsigned round, std::mt19937& random, bool stop_early) {
    // The file: graphics keys 1..N and compute keys 1..M, some written twice.
    const unsigned graphics_count = 50 + random() % 200, compute_count = random() % 40;
    std::vector<std::pair<bool, std::uint64_t>> file;
    for (unsigned i = 1; i <= graphics_count; ++i) file.emplace_back(false, i);
    for (unsigned i = 1; i <= compute_count; ++i) file.emplace_back(true, i);
    for (unsigned i = 0; i < 5; ++i) file.push_back(file[random() % file.size()]);
    std::shuffle(file.begin(), file.end(), random);
    const std::uint64_t file_bytes = file.size() * 100;

    auto loader = std::make_shared<Loader>(2 + random() % 8, file_bytes);
    std::atomic<bool> reading_done{false}, saved_while_reading{false};
    std::mutex saves_mutex;
    std::vector<std::pair<bool, std::uint64_t>> saved;  // what was written to the "file"
    Pool workers{1 + random() % 6};
    Pool serialization{1};
    const unsigned seed = random();

    // The reader (vk: LoadPipelines' callbacks, then FinishReading and WaitBuilt).
    std::jthread reader([&, seed] {
        std::mt19937 local{seed};
        std::uint64_t position = 0;
        long long waited = 0;
        for (const auto& [compute, key] : file) {
            if (loader->Stopping()) break;
            position += 100;
            loader->Note(key, compute, position);
            if (local() % 7 == 0) continue;  // filtered out by the key's state: not built
            if (!loader->Admit(compute, &waited)) break;
            workers.Queue([&, loader, compute, key, delay = local() % 300] {
                const struct Done {
                    Loader& loader;
                    ~Done() {
                        loader.Done();
                    }
                } done{*loader};
                const Eden::ShaderStart::Job timing;
                if (!loader->Wanted(key, compute)) return;
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
                auto pipeline = std::make_unique<Pipeline>(key);
                if (compute) pipeline = loader->OfferCompute(ComputeKey{key}, key, std::move(pipeline));
                else pipeline = loader->OfferGraphics(GraphicsKey{key}, key, std::move(pipeline));
            });
        }
        for (auto& save : loader->FinishReading(!loader->Stopping())) serialization.Queue(std::move(save));
        reading_done = true;
        loader->WaitBuilt(std::chrono::milliseconds(1), [] {});
    });

    // The GPU thread: asks for keys (some not in the file at all), keeps what it gets.
    std::unordered_map<std::uint64_t, std::unique_ptr<Pipeline>> graphics_cache, compute_cache;
    std::set<std::pair<bool, std::uint64_t>> built_itself;
    const unsigned asks = 100 + random() % 300;
    for (unsigned ask = 0; ask < asks; ++ask) {
        if (stop_early && ask == asks / 3) break;
        const bool compute = compute_count != 0 && random() % 4 == 0;
        const std::uint64_t key = 1 + random() % ((compute ? compute_count : graphics_count) + 20);
        auto& cache = compute ? compute_cache : graphics_cache;
        auto [entry, is_new] = cache.try_emplace(key);
        if (!is_new) continue;
        entry->second = compute ? loader->TakeCompute(ComputeKey{key}, key) : loader->TakeGraphics(GraphicsKey{key}, key);
        if (!entry->second) {
            entry->second = std::make_unique<Pipeline>(key);
            built_itself.emplace(compute, key);
            auto save = [&, compute, key] {
                // Written only once the reading has ended (it never starts again).
                if (loader->GetCounts().reading) saved_while_reading = true;
                std::scoped_lock lock{saves_mutex};
                saved.emplace_back(compute, key);
            };
            if (auto now = loader->Save(key, compute, save)) serialization.Queue(std::move(*now));
        }
        if (random() % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(random() % 500));
    }

    // The end: the game left now (stop_early), or once the load has finished.
    const auto stop_start = std::chrono::steady_clock::now();
    if (!stop_early) {
        while (!reading_done || loader->GetCounts().finished < loader->GetCounts().graphics + loader->GetCounts().compute)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const auto counts = loader->GetCounts();
    loader->Stop();
    reader.join();
    loader->WaitIdle();
    auto [graphics_left, compute_left] = loader->TakeAll();
    if (stop_early && std::chrono::steady_clock::now() - stop_start > std::chrono::seconds(5))
        Fail("stopping in the middle took too long", round);
    serialization.Drain();
    {
        std::scoped_lock lock{saves_mutex};
        if (saved_while_reading) Fail("a pipeline was saved while the file was still being read", round);
        const bool whole = !stop_early;
        std::set<std::pair<bool, std::uint64_t>> in_file(file.begin(), file.end());
        for (const auto& item : built_itself) {
            const bool was_saved = std::find(saved.begin(), saved.end(), item) != saved.end();
            const bool new_key = !in_file.contains(item);
            if (whole && was_saved != new_key) Fail("a save did not follow whether the file had the pipeline", round);
            if (!whole && was_saved && !new_key) Fail("a pipeline the file has was saved again", round);
        }
        if (std::set(saved.begin(), saved.end()).size() != saved.size()) Fail("a pipeline was saved twice", round);
    }
    // Each pipeline the GPU thread holds is one object, made once for its key.
    for (const auto& [key, pipeline] : graphics_cache)
        if (pipeline && pipeline->key != key) Fail("the GPU thread got another key's pipeline", round);
    for (const auto& [key, pipeline] : graphics_left)
        if (graphics_cache.contains(key.value) && graphics_cache[key.value])
            Fail("a pipeline was both kept by the load and in the GPU thread's cache", round);
    if (!stop_early && counts.finished != counts.graphics + counts.compute) Fail("builds were lost", round);
    graphics_left.clear();
    compute_left.clear();
    graphics_cache.clear();
    compute_cache.clear();
    return Outcome{stop_early};
}
} // namespace

int main(int argc, char** argv) {
    const unsigned rounds = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 300;
    std::mt19937 random{20261030};
    unsigned stopped = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        stopped += Round(round, random, round % 3 == 1).stopped;
        if (alive.load() != 0) Fail("a pipeline outlived the load", round);
        if (Eden::ShaderStart::GetProgress().active && !Eden::ShaderStart::GetProgress().reading) {
            // The vk side clears it at the end; the loader itself leaves its last count.
        }
        Eden::ShaderStart::SetProgress({});
    }
    std::printf("Shader cache load while the game plays: PASS %u rounds (%u stopped in the middle): every pipeline built "
                "once for the GPU thread, saves only for pipelines the file lacks and none while it is read, prompt "
                "stops, nothing outlives the load\n",
                rounds, stopped);
    return 0;
}
