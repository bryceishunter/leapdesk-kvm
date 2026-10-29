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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace inputleap {

//! Fetches files copied on another screen, for a paste
/*!
Asks the screen that copied the files to send them with Taildrop, waits for it
to say they've been sent, waits for each to arrive in the folder Taildrop saves
files in, and moves it, under its own name, into a folder of its own in the
staging folder.  One fetch at a time.
*/
class MSWindowsFileFetch {
public:
    //! How a fetch is going, for a progress window
    struct Progress {
        enum class Stage { WAITING, SENDING, DONE, FAILED };
        std::string request;
        Stage stage = Stage::WAITING;
        std::size_t files = 0;      //!< how many files are being pasted
        std::string name;           //!< the first one's name
        std::uint64_t sent = 0;     //!< bytes sent so far, when the sender says
        std::uint64_t total = 0;    //!< bytes in all
        bool counting = false;      //!< whether the sender says how much it has sent
        std::string detail;         //!< why it failed
    };

    //! The failure of a fetch that was cancelled
    static constexpr const char* kCancelled = "the paste was cancelled";

    struct Settings {
        //! Asks the screen that copied the files to send them
        std::function<void(const FilePasteRequest&)> request;
        //! This machine's Tailscale address, which the files are sent to
        std::function<std::string()> own_address;
        //! The folder the files arrive in
        std::function<std::wstring()> received_folder;
        //! Where fetched files are kept under their own names
        std::wstring staging_folder;
        //! How long the source has to start sending, the files to show up
        //! once sent (and to be let go of), and the whole transfer to take
        std::chrono::milliseconds answer_timeout{std::chrono::seconds(30)};
        std::chrono::milliseconds arrival_timeout{std::chrono::seconds(30)};
        std::chrono::milliseconds transfer_timeout{std::chrono::hours(1)};
        //! How long a sender that reports progress may go quiet before the
        //! transfer counts as stuck
        std::chrono::milliseconds stall_timeout{std::chrono::minutes(1)};
        //! Told how the fetch is going, on the thread fetching
        std::function<void(const Progress&)> progress;
    };

    explicit MSWindowsFileFetch(Settings settings);

    //! Fetches the files of \p clip for \p requester (for the log): blocks
    //! until they're in the staging folder, then returns their paths there in
    //! order; or false and why
    bool fetch(const FileClip& clip, const std::string& requester,
               std::vector<std::wstring>& paths, std::string& failure);

    //! Notify of progress of the fetch that is waiting
    void status(const FilePasteStatus& status);

    //! Set whether the other screens can be reached; while they can't, a
    //! fetch fails at once instead of waiting
    void set_connected(bool connected);

    //! Fails the fetch for paste \p request, if that's the one waiting, with
    //! kCancelled; returns whether it was
    bool cancel(const std::string& request);

    //! Fails a fetch in progress, and any after it
    void stop();

    const Settings& settings() const { return settings_; }

private:
    bool wait_for_files(const Progress& start, std::string& failure);
    bool collect(const FileClip& clip, const std::string& request,
                 std::vector<std::wstring>& paths, std::string& failure);
    void tell(const Progress& progress);

    Settings settings_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    bool connected_ = true;
    bool cancelled_ = false;
    // the fetch waiting for its files, and what has been heard of it
    std::string waiting_;
    std::vector<FilePasteStatus> statuses_;
};

} // namespace inputleap
