#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of a game played where it lies on its download source (headless/streamed_game.h).

Builds headless/stream_vfs.h, the file Eden reads such a game through, with
headless/remote/stream_file.cpp, the pinned Eden's own file layer (src/core/file_sys/vfs/vfs.cpp)
and a stand-in source, under ASan/UBSan and ThreadSanitizer, and checks what Eden gets: the
file's name, size and extension, every byte through Read and through Eden's own ReadBytes,
ReadByte and ReadAllBytes, nothing written; its folder, empty, which Eden's loaders look into
without asking whether there is one (the unpacked-game loader's IdentifyType, checked to be so in
the pinned Eden), also when its source failed; a source that fails for good makes the reads come
short before the game runs, says so once in the log and tells the session why, and once the game
runs (Hold) makes them wait, telling the session at once and once, until the game is stopped
(Stop); a file stopped for a game that stops is no failure; a read that waits long for the source
is said in the log; the session finds the game by its path and by no other.

The patch of Eden's GetGameFileFromPath (headless/CMakeLists.txt): the text it is made at is
still in the pinned Eden's src/core/core.cpp, once; patched as the build patches it, the
function looks the path up first; and the patched lines themselves, built here, give the
streamed game's file for its path, nothing for another streamed path, and leave every other
path to Eden's own code. A build's own derived core.cpp is checked as well when there is one.

The launcher (headless/prosperoeden/pe/ui): what playing a game starts (play_file): a game on the
console its file, one on download sources only its streamed path when a source of it streams
(Game::stream, which the launcher's service sets only for those, remote_check.cpp), else nothing,
so it is downloaded first; and the Library's, the home screen's and the launcher packs' play
actions start what play_file says.

The pinned Eden's sources come from .deps/mirror-<commit>/ when it is there, else from the
build's unpacked tree, else from .deps/eden-<commit>.tar.gz (checked against UPSTREAM.json).
Without any of them the check says so and is skipped.
"""
from pathlib import Path
import hashlib, json, os, platform, re, shutil, subprocess, sys, tarfile, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
upstream = json.loads((root / 'UPSTREAM.json').read_text())
commit = upstream['eden_commit']


def eden_sources(work):
    """The pinned Eden's src/ folder (from the mirror, the build's tree or the archive), or None."""
    mirror = root / '.deps' / f'mirror-{commit}' / 'src'
    if (mirror / 'core/core.cpp').is_file():
        return mirror
    cache = root / '.local/headless-cache'
    if cache.is_file():
        unpacked = Path(cache.read_text().strip()) / 'source'
        if (unpacked / '.unpacked-from').is_file() and (unpacked / 'src/core/core.cpp').is_file() and \
                (unpacked / '.unpacked-from').read_text().strip() == f'eden-{commit[:8]}.tar.gz':
            return unpacked / 'src'
    archive = root / '.deps' / f'eden-{commit[:8]}.tar.gz'
    if not archive.is_file():
        return None
    if hashlib.sha256(archive.read_bytes()).hexdigest() != upstream['archives'][archive.name]:
        sys.exit(f'{archive} is not the pinned archive (UPSTREAM.json)')
    wanted = ('src/common/', 'src/core/file_sys/', 'src/core/core.cpp', 'src/core/loader/')
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            name = member.name.split('/', 1)[1] if '/' in member.name else ''
            if member.isfile() and name.startswith(wanted) and '..' not in Path(name).parts:
                member.name = name
                members.append(member)
        tar.extractall(work / 'eden', members=members)
    return work / 'eden/src'


def cmake_string(name):
    """A set(<name> "...") of headless/CMakeLists.txt, its escapes read."""
    text = (headless / 'CMakeLists.txt').read_text()
    found = re.findall(r'set\(' + name + r' "((?:[^"\\]|\\.)*)"\)', text)
    assert len(found) == 1, f'headless/CMakeLists.txt sets {name} once'
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), found[0])


def run(exe):
    """Runs a check's program. Under a kernel that places memory more randomly than an older
    ThreadSanitizer knows (Ubuntu 24.04's with GCC 13: "unexpected memory mapping"), the same
    program without address randomization."""
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    if result.returncode != 0 and 'unexpected memory mapping' in result.stderr and shutil.which('setarch'):
        result = subprocess.run(['setarch', platform.machine(), '-R', str(exe)], capture_output=True, text=True,
                                timeout=600)
    return result


launcher_code = r'''
#include "pe/ui/launch_commit.hpp"
#include <cassert>
#include <cstdio>
int main() {
    pe::ui::Game local;
    local.file = "Alpha Quest/Alpha Quest.nsp";
    local.sources = {"Home"};
    pe::ui::Game download = local;
    download.remote = true;
    pe::ui::Game streamed = download;
    streamed.stream = "stream:home/10";
    pe::ui::Game on_console = local; // a game on the console is played from its file, whatever else
    on_console.stream = "stream:home/10";
    assert(pe::ui::play_file(local) == "Alpha Quest/Alpha Quest.nsp");
    assert(pe::ui::play_file(download).empty());
    assert(pe::ui::play_file(streamed) == "stream:home/10");
    assert(pe::ui::play_file(on_console) == "Alpha Quest/Alpha Quest.nsp");
    std::puts("ok");
}
'''

code = r'''
#include "stream_vfs.h"
#include "streamed_game.h"
#include "common/fs/path_util.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
using namespace Eden;
using Remote::StreamFile;
static std::uint8_t Byte(std::uint64_t at) { return static_cast<std::uint8_t>((at * 2654435761u) >> 13); }

// The path helpers of Eden's common library that vfs.cpp names (src/common/fs/path_util.cpp needs
// the rest of that library): a file read where it lies has no path, and none of them is used here.
namespace Common::FS {
std::vector<std::string_view> SplitPathComponents(std::string_view) { return {}; }
std::string SanitizePath(std::string_view path, DirectorySeparator) { return std::string{path}; }
std::string GetParentPath(std::string_view) { return {}; }
std::string_view GetPathWithoutTop(std::string_view) { return {}; }
}

// A source with a file of known bytes, sent from any offset, as a network does.
struct FakeSource final : Remote::Source {
    std::uint64_t size;
    std::atomic<int> fail_next{0};             // fetches that bring nothing
    std::uint64_t stall_from = 0;              // the bytes from here on never come (0: they all do)
    std::chrono::milliseconds answer{0};       // how long each fetch takes to begin
    bool from_start_only = false;              // it sends the file only from its start
    std::uint64_t told_length = 0;             // the length it says follows (0: the right one)
    explicit FakeSource(std::uint64_t size_) : size(size_) {}
    std::string address() const override { return "fake"; }
    bool list(std::vector<Remote::SourceGame>*, std::string*, const Remote::Stopped&) override { return false; }
    bool cover(const Remote::SourceGame&, std::string*, const Remote::Stopped&) override { return false; }
    bool fetch(const Remote::SourceGame&, const Remote::SourceFile&, std::uint64_t offset, Remote::Receiver& receiver,
               std::string* error) override {
        if (fail_next > 0) { --fail_next; *error = "connection reset"; return false; }
        std::this_thread::sleep_for(answer);
        if (from_start_only) offset = 0;
        if (!receiver.begin(from_start_only, told_length != 0 ? told_length : size - offset)) { *error = "stopped"; return false; }
        std::uint8_t chunk[64 * 1024];
        for (std::uint64_t at = offset; at < size;) {
            if (receiver.stopped()) { *error = "stopped"; return false; }
            if (stall_from > 0 && at >= stall_from) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const std::size_t part = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(chunk), size - at));
            for (std::size_t i = 0; i < part; ++i) chunk[i] = Byte(at + i);
            if (!receiver.take(chunk, part)) { *error = "stopped"; return false; }
            at += part;
        }
        return true;
    }
};

struct Said {
    std::mutex lock;
    std::vector<std::string> lines;
    StreamVfsFile::Log log() {
        return [this](const std::string& line) { const std::lock_guard held(lock); lines.push_back(line); };
    }
    std::size_t count(const std::string& part) {
        const std::lock_guard held(lock);
        std::size_t n = 0;
        for (const std::string& line : lines) n += line.find(part) != std::string::npos;
        return n;
    }
};

constexpr std::uint64_t kSize = 5 * StreamFile::kBlock + 123;
const Remote::SourceFile kFile{"7", "Game [0100000000010000][v0].nsp", kSize, Remote::FileKind::game};

std::shared_ptr<StreamFile> Open(std::unique_ptr<FakeSource> source) {
    return std::make_shared<StreamFile>(std::move(source), Remote::SourceGame{}, kFile, 40);
}
bool Same(const std::vector<std::uint8_t>& got, std::uint64_t offset) {
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i] != Byte(offset + i)) return false;
    return true;
}

// The function the build patches (headless/CMakeLists.txt), its own body left to this stand-in.
static int fallbacks = 0;
static FileSys::VirtualFile EdenOwn(const FileSys::VirtualFilesystem&, const std::string&) {
    ++fallbacks;
    return nullptr;
}
namespace Core {
@PATCHED@    return EdenOwn(vfs, path);
}
}

int main() {
    Said said;
    {
        // What Eden sees of the file, and every byte of it, however Eden asks.
        std::shared_ptr<StreamFile> file = Open(std::make_unique<FakeSource>(kSize));
        FileSys::VirtualFile vfs = std::make_shared<StreamVfsFile>(file, said.log());
        assert(vfs->GetName() == kFile.name && vfs->GetExtension() == "nsp" && vfs->GetSize() == kSize);
        assert(vfs->IsReadable() && !vfs->IsWritable() && vfs->GetFullPath() == "/" + kFile.name);
        // Its folder holds nothing: as IsDirectoryExeFS asks it, it is no unpacked game.
        const FileSys::VirtualDir folder = vfs->GetContainingDirectory();
        assert(folder != nullptr && folder->GetFile("main") == nullptr && folder->GetFile("main.npdm") == nullptr &&
               folder->GetFiles().empty() && folder->GetSubdirectories().empty());
        const std::uint8_t byte = 1;
        assert(vfs->Write(&byte, 1, 0) == 0 && !vfs->WriteByte(1, 0) && !vfs->Resize(1) && !vfs->Rename("other.nsp"));
        std::vector<std::uint8_t> got(300000);
        assert(vfs->Read(got.data(), got.size(), 0) == got.size() && Same(got, 0));
        assert(vfs->Read(got.data(), got.size(), 2 * StreamFile::kBlock - 1000) == got.size() &&
               Same(got, 2 * StreamFile::kBlock - 1000));
        const auto tail = vfs->ReadBytes(1000, kSize - 10);
        assert(tail.size() == 10 && Same(tail, kSize - 10));
        assert(vfs->ReadByte(3) == Byte(3) && !vfs->ReadByte(kSize).has_value());
        const auto all = vfs->ReadAllBytes();
        assert(all.size() == kSize && Same(all, 0));
        assert(static_cast<StreamVfsFile&>(*vfs).Failed().empty());
    }
    // Let go: what its transfers came to, one line; nothing failed.
    assert(said.count("Closed after ") == 1 && said.count(" transfers, ") == 1 && said.count("fail from now on") == 0);
    said.lines.clear();
    {
        // A source that fails for good: the reads come short, from several threads, the log says
        // so once, and the session is told why.
        auto source = std::make_unique<FakeSource>(kSize);
        source->fail_next = 1000;
        auto vfs = std::make_shared<StreamVfsFile>(Open(std::move(source)), said.log());
        std::vector<std::thread> readers;
        for (unsigned t = 0; t < 4; ++t)
            readers.emplace_back([&vfs, t] {
                std::vector<std::uint8_t> got(4096);
                assert(vfs->Read(got.data(), got.size(), (t + 1) * StreamFile::kBlock) == 0);
            });
        for (auto& reader : readers) reader.join();
        assert(vfs->Failed() == "connection reset");
        assert(said.count("The game's reads of what is not kept fail from now on: connection reset") == 1);
        // What Eden's loaders ask of it then still holds (the unpacked-game loader asks its folder).
        const FileSys::VirtualDir folder = vfs->GetContainingDirectory();
        assert(folder != nullptr && folder->GetFile("main") == nullptr && folder->GetFile("main.npdm") == nullptr);
    }
    said.lines.clear();
    {
        // Once the game runs: a read its source can no longer answer waits until the game is
        // stopped, the session told at once and once; before, it came back short. After the stop
        // every read comes back with what is kept, and none waits.
        auto source = std::make_unique<FakeSource>(kSize);
        source->from_start_only = true; // fails for good at once at a read past the start
        auto vfs = std::make_shared<StreamVfsFile>(Open(std::move(source)), said.log());
        std::vector<std::uint8_t> got(1000);
        assert(vfs->Read(got.data(), got.size(), 2 * StreamFile::kBlock) == 0 && !vfs->Failed().empty());
        std::atomic<int> lost{0};
        vfs->Hold([&] { ++lost; });
        std::atomic<int> back{0};
        std::vector<std::thread> readers;
        for (unsigned t = 0; t < 2; ++t)
            readers.emplace_back([&vfs, &back, t] {
                std::vector<std::uint8_t> bytes(1000);
                assert(vfs->Read(bytes.data(), bytes.size(), (3 + t) * StreamFile::kBlock) == 0);
                ++back;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(back == 0 && lost == 1);
        vfs->Stop();
        for (auto& reader : readers) reader.join();
        assert(back == 2 && lost == 1);
        assert(vfs->Read(got.data(), got.size(), 5 * StreamFile::kBlock) == 0 && lost == 1);
        assert(said.count("fail from now on") == 1);
    }
    said.lines.clear();
    {
        // Closed while a read waits for a source that stalled (a game that stops): the read ends at
        // once, short, and that is no failure.
        auto source = std::make_unique<FakeSource>(kSize);
        source->stall_from = StreamFile::kBlock;
        std::shared_ptr<StreamFile> file = Open(std::move(source));
        auto vfs = std::make_shared<StreamVfsFile>(file, said.log());
        std::size_t read = 1;
        std::thread reader([&] {
            std::vector<std::uint8_t> got(1000);
            read = vfs->Read(got.data(), got.size(), 3 * StreamFile::kBlock);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        vfs->Stop();
        reader.join();
        assert(read == 0 && vfs->Failed().empty() && said.count("fail from now on") == 0);
    }
    said.lines.clear();
    {
        // A source slow to answer: a read that waits long is said; the next within ten seconds is not.
        auto source = std::make_unique<FakeSource>(kSize);
        source->answer = std::chrono::milliseconds(700);
        auto vfs = std::make_shared<StreamVfsFile>(Open(std::move(source)), said.log());
        std::vector<std::uint8_t> got(1000);
        assert(vfs->Read(got.data(), got.size(), 0) == got.size() && Same(got, 0));
        assert(vfs->Read(got.data(), got.size(), 4 * StreamFile::kBlock + 7) == got.size() &&
               Same(got, 4 * StreamFile::kBlock + 7));
        assert(said.count("A read of 1 KiB at 0 MiB waited 0.") == 1 && said.count("A read of") == 1);
    }
    {
        // The session's registry: the game by its path, and by no other; gone once it ended.
        auto vfs = std::make_shared<StreamVfsFile>(Open(std::make_unique<FakeSource>(kSize)));
        auto game = std::make_shared<StreamedGame::Game>();
        game->path = "stream:home/10";
        game->source = "Home";
        game->file = vfs;
        game->failed = [vfs] { return vfs->Failed(); };
        assert(StreamedGame::IsPath("stream:home/10") && !StreamedGame::IsPath("roms/Game.nsp") &&
               !StreamedGame::IsPath("/data/games/roms/stream:home/10"));
        assert(StreamedGame::Find("stream:home/10") == nullptr);
        StreamedGame::Set(game);
        assert(StreamedGame::Find("stream:home/10") == game && StreamedGame::File("stream:home/10") == vfs);
        assert(StreamedGame::Find("stream:home/11") == nullptr && StreamedGame::File("roms/Game.nsp") == nullptr);
        assert(StreamedGame::LostMessage(*game, "The server did not answer") ==
               "The game's download source Home stopped sending it (The server did not answer). Check the network "
               "and the server, then start the game again");
        // Eden's GetGameFileFromPath as patched: the streamed game's file for its path, nothing for
        // another streamed path (never a host file of that name), Eden's own way for the rest.
        const FileSys::VirtualFilesystem none;
        assert(Core::GetGameFileFromPath(none, "stream:home/10") == vfs && fallbacks == 0);
        assert(Core::GetGameFileFromPath(none, "stream:home/11") == nullptr && fallbacks == 0);
        assert(Core::GetGameFileFromPath(none, "/data/games/roms/Game.nsp") == nullptr && fallbacks == 1);
        StreamedGame::Close();
        assert(StreamedGame::Find("stream:home/10") == nullptr && Core::GetGameFileFromPath(none, "stream:home/10") == nullptr);
    }
    std::puts("ok");
}
'''

with tempfile.TemporaryDirectory(prefix='streamed-game-') as tmp:
    work = Path(tmp)
    eden = eden_sources(work)
    if eden is None:
        print('check-streamed-game: SKIPPED (no pinned Eden source: .deps/mirror-*, the build tree or '
              f'.deps/eden-{commit[:8]}.tar.gz)')
        sys.exit(0)
    # Eden's loader of unpacked games asks the folder of any file it is shown, without looking
    # whether there is one (why the streamed file has an empty folder of its own).
    unpacked = (eden / 'core/loader/deconstructed_rom_directory.cpp').read_text()
    exefs = (eden / 'core/file_sys/content_archive.h').read_text()
    assert 'FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory())' in unpacked and \
        'return pfs->GetFile("main") != nullptr && pfs->GetFile("main.npdm") != nullptr;' in exefs, \
        "the pinned Eden's unpacked-game loader looks into a file's folder"
    # The build's patch of GetGameFileFromPath, at the text it is made at.
    core = (eden / 'core/core.cpp').read_text()
    old, new = cmake_string('game_file_old'), cmake_string('game_file_new')
    assert core.count(old) == 1, 'the pinned core.cpp has the text the build patches GetGameFileFromPath at, once'
    patched = core.replace(old, old + new)
    body = patched.split(old, 1)[1]
    assert body.startswith(new) and 'Common::SplitPath(path' in body.split('\n}\n', 1)[0], \
        "patched, GetGameFileFromPath looks the path up before Eden's own code"
    cmake = (headless / 'CMakeLists.txt').read_text()
    block = cmake.split('file(READ "${PROJECT_SOURCE_DIR}/src/core/core.cpp" system_source)', 1)[1].split(
        'write_derived("${PORT_BUILD_DIR}/core.cpp" "${system_source}")', 1)[0]
    assert 'string(PREPEND system_source "#include \\"${EDEN_PORT_DIR}/streamed_game.h\\"\\n")' in block and \
        'Pinned GetGameFileFromPath changed' in block, 'the patch includes streamed_game.h and stops the build without its text'
    cache = root / '.local/headless-cache'
    derived = []
    if cache.is_file():
        for build in ('native-local', 'build'):
            if (made := Path(cache.read_text().strip()) / build / 'headless/core.cpp').is_file():
                assert old + new in made.read_text(), f'{made} is patched'
                derived.append(build)
    source = code.replace('@PATCHED@', old + new)
    # The launcher's play actions start what play_file says: a game on sources only is downloaded
    # first unless it is streamed, then it starts as one on the console does.
    ui = headless / 'prosperoeden/pe/ui'
    library = (ui / 'library.cpp').read_text()
    cross = library.split('    case Key::cross:\n        if (game == nullptr || !home_.setup_ready)', 1)[1].split('    default:', 1)[0]
    assert 'if (game->remote && play_file(*game).empty())' in cross and 'play_remote(*game);' in cross and \
        'launch(play_file(*game), game->name, game->cover);' in cross, 'the Library plays what play_file says'
    home = (ui / 'home.cpp').read_text()
    assert home.count('if (game.remote && play_file(game).empty())') == 1 and \
        home.count('launch(play_file(game), game.name, game.cover);') == 1 and \
        home.count('launch(play_file(*game), game->name, game->cover);') == 1, 'the home screen plays what play_file says'
    runtime = (ui / 'runtime_launcher.cpp').read_text()
    request = runtime.split('else if (name == "launch.request") {', 1)[1].split('else if (name == "launch.cancel")', 1)[0]
    assert 'const std::string file=play_file(g); if(file.empty() ||' in request and 'launch_file=file;' in request, \
        "a launcher pack's launch.request starts what play_file says"
    exe = work / 'play-file'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(headless / 'prosperoeden'),
                    '-x', 'c++', '-', '-o', str(exe)], input=launcher_code, text=True, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    if result.returncode != 0 or result.stdout.strip() != 'ok':
        sys.exit('play_file check failed: ' + result.stderr[-2000:])
    print('[launcher] ok')
    for flags, label in ((['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'], 'checked'),
                         (['-O1', '-g', '-fsanitize=thread'], 'thread sanitizer')):
        compiler = os.environ.get('CXX', 'c++')
        objects = []
        for unit in ('vfs', 'vfs_vector'):
            objects.append(work / f'{unit}-{label.split()[0]}.o')
            subprocess.run([compiler, '-std=c++20', '-w', *flags, '-I' + str(eden), '-c',
                            str(eden / f'core/file_sys/vfs/{unit}.cpp'), '-o', str(objects[-1])], check=True)
        exe = work / ('streamed-' + label.split()[0])
        subprocess.run([compiler, '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', *flags, '-I' + str(headless),
                        '-I' + str(headless / 'remote'), '-isystem', str(eden), '-x', 'c++', '-', '-x', 'none',
                        str(headless / 'remote/stream_file.cpp'), *map(str, objects), '-o', str(exe)],
                       input=source, text=True, check=True)
        result = run(exe)
        if result.returncode != 0 or result.stdout.strip() != 'ok':
            sys.stderr.write(result.stdout[-2000:] + result.stderr[-3000:])
            sys.exit(f'streamed game check failed ({label}): exit {result.returncode}')
        print(f'[{label}] ok')
print('Streamed game: PASS Eden reads every byte of a game where it lies through its own file layer, nothing is '
      'written, its folder is empty (the unpacked-game loader looks into it), a failed source fails the reads and '
      'says why once, once the game runs its reads wait for the stop instead and the session is told at once, a stop '
      'is no failure, a slow read is said, the '
      "session finds the game by its path alone; GetGameFileFromPath's patch text is in the pinned Eden once and "
      'looks the path up first' + (f' (derived core.cpp of {", ".join(derived)} too)' if derived else '') +
      '; the launcher plays a game on sources only where it lies only when a source of it streams, else downloads it')
