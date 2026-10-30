// SPDX-License-Identifier: GPL-3.0-or-later
// Player 1's NFC reader (amiibo.h). The tag handling follows Eden's InputCommon::VirtualAmiibo
// (input_common/drivers/virtual_amiibo.cpp, GPL-3.0-or-later; Copyright 2022 yuzu Emulator Project).
#include "amiibo.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include "common/input.h"
#include "diagnostics.h"
#include "input_common/input_poller.h"

namespace Eden {
namespace {
// The one reader: player 1's controller (Eden's "engine:virtual_amiibo,nfc:1": no guid, port 0, pad 0).
const PadIdentifier reader{};

bool DumpSize(std::uintmax_t size) {
    return size == AmiiboEngine::kAmiiboSize || size == AmiiboEngine::kAmiiboSizeWithoutPassword ||
           size == AmiiboEngine::kAmiiboSizeWithSignature || size == AmiiboEngine::kMifareSize;
}

bool IsDumpName(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string name = path.filename().string();
    return extension == ".bin" && !name.empty() && name.front() != '.';
}
} // namespace

AmiiboEngine::AmiiboEngine(std::string name, std::vector<std::string> folders_, std::chrono::milliseconds tap_delay_)
    : InputEngine(std::move(name)), folders(std::move(folders_)), tap_delay(tap_delay_) {
    // The tag is set on the reader before any device asks for it (SetNfc needs it listed).
    PreSetController(reader);
    tapper = std::thread([this] { Tapper(); });
}

AmiiboEngine::~AmiiboEngine() {
    {
        const std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    if (tapper.joinable()) tapper.join();
}

std::vector<std::string> AmiiboEngine::Dumps() const {
    std::vector<std::string> dumps;
    for (const std::string& folder : folders) {
        std::error_code error;
        for (std::filesystem::directory_iterator it{folder, error}, end; !error && it != end; it.increment(error)) {
            std::error_code file_error;
            if (!it->is_regular_file(file_error) || !IsDumpName(it->path())) continue;
            const auto size = it->file_size(file_error);
            if (!file_error && DumpSize(size)) dumps.push_back(it->path().string());
        }
    }
    // Name order within the folders, the folders in the order given; one name once.
    std::stable_sort(dumps.begin(), dumps.end(), [](const std::string& a, const std::string& b) {
        return std::filesystem::path(a).filename() < std::filesystem::path(b).filename();
    });
    dumps.erase(std::unique(dumps.begin(), dumps.end(),
                            [](const std::string& a, const std::string& b) {
                                return std::filesystem::path(a).filename() == std::filesystem::path(b).filename();
                            }),
                dumps.end());
    return dumps;
}

std::string AmiiboEngine::LastShown() const {
    const std::lock_guard lock(mutex);
    return shown;
}

AmiiboEngine::State AmiiboEngine::GetState() const {
    const std::lock_guard lock(mutex);
    return state;
}

// The engine's callbacks reach Eden's controller and NFC service, which may call back here: they
// are made without the lock, with a copy of the status taken under it.
void AmiiboEngine::Notify(std::unique_lock<std::mutex>& lock, Common::Input::NfcStatus value) {
    lock.unlock();
    SetNfc(reader, value);
    lock.lock();
}

Common::Input::DriverResult AmiiboEngine::SetPollingMode(const PadIdentifier&,
                                                         Common::Input::PollingMode polling_mode) {
    std::unique_lock lock(mutex);
    if (polling_mode == Common::Input::PollingMode::NFC) {
        // Already reading (a game that asks again): the tag stays.
        if (state == State::Disabled) state = State::Initialized;
        return Common::Input::DriverResult::Success;
    }
    if (state == State::TagNearby) RemoveTag(lock);
    state = State::Disabled;
    wake.notify_all();
    return Common::Input::DriverResult::NotSupported;
}

Common::Input::NfcState AmiiboEngine::SupportsNfc(const PadIdentifier&) const {
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::StartNfcPolling(const PadIdentifier&) {
    std::unique_lock lock(mutex);
    if (state != State::Initialized) return Common::Input::NfcState::WrongDeviceState;
    state = State::WaitingForAmiibo;
    waiting_since = std::chrono::steady_clock::now();
    wake.notify_all();
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::StopNfcPolling(const PadIdentifier&) {
    std::unique_lock lock(mutex);
    if (state == State::Disabled) return Common::Input::NfcState::WrongDeviceState;
    if (state == State::TagNearby) RemoveTag(lock);
    state = State::Initialized;
    wake.notify_all();
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::ReadAmiiboData(const PadIdentifier&, std::vector<u8>& out_data) {
    const std::lock_guard lock(mutex);
    if (state != State::TagNearby) return Common::Input::NfcState::WrongDeviceState;
    if (status.tag_type != 1U << 1) return Common::Input::NfcState::InvalidTagType;
    out_data = data;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::WriteNfcData(const PadIdentifier&, const std::vector<u8>& bytes) {
    const std::lock_guard lock(mutex);
    if (state != State::TagNearby || shown.empty()) return Common::Input::NfcState::WrongDeviceState;
    // Over the start of the dump, as Eden writes it (a signature after the tag stays), beside it
    // first and renamed over it, so a session that ends mid-write keeps the figure.
    std::vector<u8> whole;
    {
        std::ifstream in(shown, std::ios::binary);
        whole.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (whole.size() < bytes.size()) whole.resize(bytes.size());
    std::copy(bytes.begin(), bytes.end(), whole.begin());
    const std::string temporary = shown + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(whole.data()), static_cast<std::streamsize>(whole.size()));
        if (!out.good()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            Report("amiibo", ("could not write " + shown).c_str());
            return Common::Input::NfcState::WriteFailed;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, shown, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        Report("amiibo", ("could not write " + shown).c_str());
        return Common::Input::NfcState::WriteFailed;
    }
    data = bytes;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::ReadMifareData(const PadIdentifier&, const Common::Input::MifareRequest& request,
                                                     Common::Input::MifareRequest& out_data) {
    const std::lock_guard lock(mutex);
    if (state != State::TagNearby) return Common::Input::NfcState::WrongDeviceState;
    if (status.tag_type != 1U << 6) return Common::Input::NfcState::InvalidTagType;
    for (std::size_t i = 0; i < request.data.size(); ++i) {
        if (request.data[i].command == 0) continue;
        out_data.data[i].command = request.data[i].command;
        out_data.data[i].sector = request.data[i].sector;
        const std::size_t at = request.data[i].sector * sizeof(Common::Input::MifareData::data);
        if (data.size() < at + sizeof(Common::Input::MifareData::data)) return Common::Input::NfcState::WriteFailed;
        // The sector key is not checked, as in Eden.
        std::memcpy(out_data.data[i].data.data(), data.data() + at, sizeof(Common::Input::MifareData::data));
    }
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState AmiiboEngine::WriteMifareData(const PadIdentifier&,
                                                      const Common::Input::MifareRequest& request) {
    const std::lock_guard lock(mutex);
    if (state != State::TagNearby) return Common::Input::NfcState::WrongDeviceState;
    if (status.tag_type != 1U << 6) return Common::Input::NfcState::InvalidTagType;
    for (std::size_t i = 0; i < request.data.size(); ++i) {
        if (request.data[i].command == 0) continue;
        const std::size_t at = request.data[i].sector * sizeof(Common::Input::MifareData::data);
        if (data.size() < at + sizeof(Common::Input::MifareData::data)) return Common::Input::NfcState::WriteFailed;
        std::memcpy(data.data() + at, request.data[i].data.data(), sizeof(Common::Input::MifareData::data));
    }
    return Common::Input::NfcState::Success;
}

// The figure leaves the reader (the lock is held).
void AmiiboEngine::RemoveTag(std::unique_lock<std::mutex>& lock) {
    state = State::WaitingForAmiibo;
    status.state = Common::Input::NfcState::AmiiboRemoved;
    const auto removed = status;
    status.tag_type = 0;
    Notify(lock, removed);
}

// The next dump onto the reader (the lock is held, the state WaitingForAmiibo). False with no dump.
bool AmiiboEngine::TapNext(std::unique_lock<std::mutex>& lock) {
    const std::vector<std::string> dumps = Dumps();
    for (std::size_t tried = 0; tried < dumps.size(); ++tried) {
        const std::string& path = dumps[next++ % dumps.size()];
        std::ifstream in(path, std::ios::binary);
        std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!DumpSize(bytes.size())) continue;
        const bool mifare = bytes.size() == kMifareSize;
        // An NTAG215 dump is kept at its full 540 bytes (a shorter one is padded, as in Eden).
        bytes.resize(mifare ? kMifareSize : kAmiiboSize);
        data = std::move(bytes);
        shown = path;
        status = {};
        status.tag_type = mifare ? 1U << 6 : 1U << 1;
        status.uuid_length = mifare ? 4 : 7;
        status.protocol = 1;
        status.state = Common::Input::NfcState::NewAmiibo;
        std::memcpy(status.uuid.data(), data.data(), status.uuid_length);
        state = State::TagNearby;
        Report("amiibo", ("shown to the game: " + std::filesystem::path(path).filename().string()).c_str());
        Notify(lock, status);
        return true;
    }
    return false;
}

// A scan that has waited `tap_delay` gets the next dump; one with none keeps waiting.
void AmiiboEngine::Tapper() {
    std::unique_lock lock(mutex);
    bool said_none = false;
    while (!stopping) {
        if (state != State::WaitingForAmiibo) {
            said_none = false;
            wake.wait(lock);
            continue;
        }
        const auto due = waiting_since + tap_delay;
        if (std::chrono::steady_clock::now() < due) {
            wake.wait_until(lock, due);
            continue;
        }
        if (!TapNext(lock)) {
            if (!said_none) {
                Report("amiibo", "the game is scanning; no amiibo dumps in amiibo/, so none is shown");
                said_none = true;
            }
            // Looked for again a little later: a dump copied in meanwhile is shown.
            waiting_since = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        }
    }
}

Amiibo::Amiibo(std::vector<std::string> folders, std::chrono::milliseconds tap_delay)
    : engine{std::make_shared<AmiiboEngine>("virtual_amiibo", std::move(folders), tap_delay)} {
    Common::Input::RegisterInputFactory("virtual_amiibo", std::make_shared<InputCommon::InputFactory>(engine));
    Common::Input::RegisterOutputFactory("virtual_amiibo", std::make_shared<InputCommon::OutputFactory>(engine));
}

Amiibo::~Amiibo() {
    Common::Input::UnregisterInputFactory("virtual_amiibo");
    Common::Input::UnregisterOutputFactory("virtual_amiibo");
}
} // namespace Eden
