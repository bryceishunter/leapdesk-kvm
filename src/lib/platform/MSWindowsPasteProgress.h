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

#include "platform/MSWindowsFileFetch.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace inputleap {

//! A window showing how a paste of files from another screen is going
/*!
Shows what is being pasted, a progress bar, how much has arrived, how fast and
how long is left, and a Cancel button.  It appears only once a paste has taken
a moment, so that quick ones don't flash it up, and goes when the paste ends.
It runs on a thread of its own, so it keeps responding whatever the thread
fetching the files is doing.
*/
class MSWindowsPasteProgress {
public:
    using Progress = MSWindowsFileFetch::Progress;
    //! Called on the window's thread when the user cancels paste \p request
    using Cancel = std::function<void(const std::string& request)>;

    //! The window's controls (for tests)
    enum Control { kHeading = 100, kBar = 101, kAmount = 102, kSpeed = 103, kCancel = IDCANCEL };

    explicit MSWindowsPasteProgress(Cancel cancel, std::chrono::milliseconds delay =
                                                       std::chrono::milliseconds(600));
    //! Closes the window
    ~MSWindowsPasteProgress();

    //! Notify of how a paste is going; from any thread
    void update(const Progress& progress);

    //! The window, shown or not, or null if it couldn't be made (for tests)
    HWND window() const { return window_; }
    //! Whether it has put the window up (for tests: in a window station
    //! that isn't interactive, Windows doesn't really show it)
    bool shown() const { return shown_; }

    //! A size the way Explorer shows one: "820 KB", "12.3 MB", "1.23 GB"
    static std::wstring format_size(std::uint64_t bytes);
    //! "about 5 seconds", "about 2 minutes", "about 1 hour 5 minutes"
    static std::wstring format_time_left(std::chrono::seconds left);

private:
    void run(HANDLE created);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void apply(const Progress& progress);
    void show();
    void cancel();
    void layout(UINT dpi);
    void set_text(int control, const std::wstring& text);
    void set_marquee(bool marquee);

    Cancel cancel_;
    std::chrono::milliseconds delay_;
    std::thread thread_;
    HWND window_ = nullptr;
    std::atomic<bool> shown_{false};

    // updates waiting for the window's thread
    std::mutex mutex_;
    std::deque<Progress> updates_;

    // the rest belongs to the window's thread: the paste shown (or due to be),
    // and recent amounts sent, to work out the speed from
    std::string request_;
    bool cancelling_ = false;
    bool marquee_ = false;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> samples_;
    HFONT font_ = nullptr;
    HFONT heading_font_ = nullptr;
};

} // namespace inputleap
