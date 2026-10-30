// SPDX-License-Identifier: GPL-3.0-or-later
// Player 1's NFC reader: the "virtual_amiibo" input engine Eden binds the first controller's NFC
// to (hid_core EmulatedController, "engine:virtual_amiibo,nfc:1"). Without it the engine was
// unknown, the controller had no NFC, and a game that asked for amiibo got "no device" (error
// 2115-0064 in Tears of the Kingdom) instead of its own scan screen.
//
// After Eden's InputCommon::VirtualAmiibo (input_common/drivers/virtual_amiibo.cpp, GPL-3.0-or-later),
// which a desktop frontend feeds from a file the player picks. There is no file picker here: a game
// that starts scanning is shown the next amiibo dump of the amiibo/ folders (in name order, then from
// the first again), a second later, as a player would hold a figure to the controller. With no dumps
// nothing is shown, and the game's scan screen waits until the player cancels it, as on a Switch with
// no figure at hand. A game that writes the amiibo (its own save data on the figure) writes the dump.
//
// Dumps are the console's own format: 540 bytes (NTAG215, also 532 without the password and 572
// with a signature) or 1 KiB (MIFARE). With the console's amiibo key (key_retail.bin in keys/) a
// game reads and writes everything on them; without it, what a game needs to know the figure.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "input_common/input_engine.h"

namespace Eden {
class AmiiboEngine final : public InputCommon::InputEngine {
public:
    enum class State { Disabled, Initialized, WaitingForAmiibo, TagNearby };

    // `folders`: where the dumps are looked for, each time a game starts scanning.
    AmiiboEngine(std::string name, std::vector<std::string> folders, std::chrono::milliseconds tap_delay);
    ~AmiiboEngine() override;
    AmiiboEngine(const AmiiboEngine&) = delete;
    AmiiboEngine& operator=(const AmiiboEngine&) = delete;

    Common::Input::DriverResult SetPollingMode(const PadIdentifier& identifier,
                                               Common::Input::PollingMode polling_mode) override;
    Common::Input::NfcState SupportsNfc(const PadIdentifier& identifier) const override;
    Common::Input::NfcState StartNfcPolling(const PadIdentifier& identifier) override;
    Common::Input::NfcState StopNfcPolling(const PadIdentifier& identifier) override;
    Common::Input::NfcState ReadAmiiboData(const PadIdentifier& identifier, std::vector<u8>& out_data) override;
    Common::Input::NfcState WriteNfcData(const PadIdentifier& identifier, const std::vector<u8>& data) override;
    Common::Input::NfcState ReadMifareData(const PadIdentifier& identifier, const Common::Input::MifareRequest& request,
                                           Common::Input::MifareRequest& out_data) override;
    Common::Input::NfcState WriteMifareData(const PadIdentifier& identifier,
                                            const Common::Input::MifareRequest& request) override;

    State GetState() const;
    // The dumps a scan would show now, in the order it shows them.
    std::vector<std::string> Dumps() const;
    // The dump shown last ("" for none yet).
    std::string LastShown() const;

    static constexpr std::size_t kAmiiboSize = 0x21C;
    static constexpr std::size_t kAmiiboSizeWithoutPassword = kAmiiboSize - 0x8;
    static constexpr std::size_t kAmiiboSizeWithSignature = kAmiiboSize + 0x20;
    static constexpr std::size_t kMifareSize = 0x400;

private:
    void Tapper();
    bool TapNext(std::unique_lock<std::mutex>& lock);
    void RemoveTag(std::unique_lock<std::mutex>& lock);
    void Notify(std::unique_lock<std::mutex>& lock, Common::Input::NfcStatus value);

    const std::vector<std::string> folders;
    const std::chrono::milliseconds tap_delay;
    mutable std::mutex mutex;
    std::condition_variable wake;
    State state = State::Disabled;
    std::chrono::steady_clock::time_point waiting_since{};
    std::size_t next = 0;           // the dump the next scan shows (an index into Dumps(), wrapped)
    std::string shown;              // the dump on the reader, or shown last
    std::vector<u8> data;           // its bytes
    Common::Input::NfcStatus status{};
    bool stopping = false;
    std::thread tapper;
};

// The engine registered under "virtual_amiibo" for as long as this lives (a game session).
class Amiibo final {
public:
    explicit Amiibo(std::vector<std::string> folders,
                    std::chrono::milliseconds tap_delay = std::chrono::milliseconds{1000});
    ~Amiibo();
    Amiibo(const Amiibo&) = delete;
    Amiibo& operator=(const Amiibo&) = delete;
    AmiiboEngine& Engine() { return *engine; }

private:
    std::shared_ptr<AmiiboEngine> engine;
};
} // namespace Eden
