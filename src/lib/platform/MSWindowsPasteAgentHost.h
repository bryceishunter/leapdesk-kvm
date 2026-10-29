/*
 * Leapdesk KVM -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Leapdesk KVM contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "inputleap/FileClip.h"
#include "platform/MSWindowsPasteAgent.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace inputleap {

class IClipboard;
class MSWindowsPasteChannel;
struct PasteMessage;

//! Runs the paste helper for the service, and talks to it
/*!
Starts the paste helper (MSWindowsPasteAgent) as the signed-in user, over a
named pipe only SYSTEM and that user can open, and starts it again if it
exits.  Files copied on another screen are handed to it to offer; it asks in
return for them to be sent when an application pastes them.
*/
class MSWindowsPasteAgentHost {
public:
    struct Settings {
        //! Asks the screen that copied the files to send them; called on the
        //! host's thread, so it should only post the request
        std::function<void(const FilePasteRequest&)> request;
        //! The user cancelled paste \p request, so the screen sending the
        //! files should stop; called on the host's thread, like request
        std::function<void(const std::string& request)> cancelled;
        //! The program to run as the helper: this one
        std::wstring program;
        //! The helper's options (the host fills in the pipe)
        MSWindowsPasteAgent::Options helper;
        //! How long to wait before starting the helper again; doubled each
        //! time it exits soon after starting, up to a minute
        std::chrono::milliseconds restart_delay{std::chrono::seconds(2)};
    };

    explicit MSWindowsPasteAgentHost(Settings settings);
    //! Stops the helper
    ~MSWindowsPasteAgentHost();

    //! Whether the helper is running and connected
    bool connected() const;
    //! Waits up to \p timeout for the helper to connect
    bool wait_connected(std::chrono::milliseconds timeout);

    //! Hands \p clipboard, which holds files, to the helper to offer.  False
    //! if it holds no valid FileClip or the helper isn't connected.
    bool offer(const IClipboard& clipboard);

    //! Something else is about to be put on the clipboard: an offer still on
    //! its way must not land after it
    void supersede();

    //! Notify of progress of a paste the helper is waiting for
    void status(const FilePasteStatus& status);

    //! Set whether the other screens can be reached
    void set_connected(bool connected);

    //! Whether what's on the clipboard is an offer the helper put there (or
    //! is putting there right now)
    bool owns_clipboard() const;

private:
    void run();
    bool serve(const std::wstring& pipe_name);
    HANDLE create_pipe(const std::wstring& name);
    bool start_helper(const std::wstring& pipe_name, PROCESS_INFORMATION& process);
    void handle(const PasteMessage& message);
    bool send(const PasteMessage& message);

    Settings settings_;
    std::thread thread_;
    HANDLE stop_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    // held while telling the helper whether the other screens are connected,
    // so that the latest word is the last one it hears
    std::mutex connected_mutex_;
    std::shared_ptr<MSWindowsPasteChannel> channel_;   // while the helper is connected
    bool helper_ready_ = false;
    bool others_connected_ = false;
    std::uint64_t generation_ = 0;
    // the offer sent and not yet placed, and when it was sent
    std::uint64_t in_flight_ = 0;
    std::chrono::steady_clock::time_point sent_at_;
    // the clipboard sequence number after the helper placed its last offer,
    // and the files it offered
    DWORD placed_sequence_ = 0;
    std::string placed_files_;

    bool owns_clipboard_locked() const;
};

} // namespace inputleap
