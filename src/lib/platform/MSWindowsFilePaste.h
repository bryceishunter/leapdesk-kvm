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
#include <vector>

namespace inputleap {

class IClipboard;

//! Pastes files that were copied on another screen
/*!
Offers a FileClip to Windows applications as CF_HDROP with delayed rendering,
from a window on a thread of its own.  When an application pastes, Windows
asks that window for the data and makes the application wait.  The thread
then asks the screen that copied the files to send them, waits for them to
arrive, gives them back their own names in a folder of their own, and hands
the application that list.  Only this thread waits; the rest of Leapdesk
keeps running.
*/
class MSWindowsFilePaste {
public:
    struct Settings {
        //! Asks the screen that copied the files to send them.  Called on the
        //! paste thread, so it should only post the request.
        std::function<void(const FilePasteRequest&)> request;
        //! This machine's Tailscale address, which the files are sent to
        std::function<std::string()> own_address;
        //! The folder the files arrive in
        std::function<std::wstring()> received_folder;
        //! Where pasted files are kept under their own names until the
        //! clipboard no longer offers them
        std::wstring staging_folder;
        //! How long the source has to start sending, the files to show up
        //! once sent, and the whole transfer to take
        std::chrono::milliseconds answer_timeout{std::chrono::seconds(30)};
        std::chrono::milliseconds arrival_timeout{std::chrono::seconds(30)};
        std::chrono::milliseconds transfer_timeout{std::chrono::hours(1)};
        //! How long after the files are offered asks for them get nothing.
        //! Explorer reads the file list the moment files land on the clipboard
        //! (to show cut files dimmed), and a few times more if it gets
        //! nothing; answering would fetch the files though nobody pasted.
        //! Windows asks again when something does paste.
        std::chrono::milliseconds settle_time{std::chrono::seconds(1)};
    };

    explicit MSWindowsFilePaste(Settings settings);
    //! Fails a paste in progress; the files stop being offered, the other
    //! formats stay on the clipboard
    ~MSWindowsFilePaste();

    //! Offer clipboard data
    /*!
    Puts \p clipboard, which holds a FileClip, on the Windows clipboard, with
    its other formats.  This happens on the paste thread, a moment later
    unless a later offer() or supersede() comes first.  Returns false if
    \p clipboard holds no valid FileClip.
    */
    bool offer(const IClipboard& clipboard);

    //! Drop an offer
    /*!
    Something else is about to be put on the clipboard, so an offer that is
    still on its way must not land after it.
    */
    void supersede();

    //! Notify of progress of the paste that is waiting
    void status(const FilePasteStatus& status);

    //! Set whether the other screens can be reached
    /*!
    While they can't, a paste fails at once instead of waiting.
    */
    void set_connected(bool connected);

    //! Makes CF_HDROP data listing \p paths
    static HGLOBAL make_drop(const std::vector<std::wstring>& paths);

    //! The file named \p name that has arrived in \p folder, or failing that
    //! one whose name starts with \p prefix, in case the name was changed on
    //! the way.  Empty if there is neither.
    static std::wstring find_received(const std::wstring& folder, const std::string& prefix,
                                      const std::string& name);

    //! Deletes the folders in \p staging_folder, except the one named \p keep.
    //! Files that are still open, say by a copy in progress, stay.
    static void clean_staging(const std::wstring& staging_folder, const std::wstring& keep);

    //! A new random paste request
    static std::string new_request();

private:
    struct Offer;

    // the paste thread: its window and what the window is sent
    void run();
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void place();
    void render();
    void lost_clipboard();

    // fetches the files of \p clip into the staging folder for a paste
    bool fetch(const FileClip& clip, const std::string& requester,
               std::vector<std::wstring>& paths);
    bool wait_for_files(const std::string& request, std::string& failure);
    bool collect(const FileClip& clip, const std::string& request,
                 std::vector<std::wstring>& paths, std::string& failure);

    Settings settings_;
    std::thread thread_;
    HWND window_ = nullptr;

    // bumped by every offer() and supersede(); an offer that isn't the
    // latest by the time it lands is dropped
    std::atomic<std::uint64_t> generation_{0};

    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false;
    bool stopping_ = false;
    bool connected_ = true;
    // the offer on its way to the paste thread
    std::unique_ptr<Offer> pending_;
    // the files on offer while our window owns the clipboard
    std::unique_ptr<FileClip> offered_;
    // when they were put there
    std::chrono::steady_clock::time_point offered_at_;
    // the paste waiting for its files, and what has been heard of it
    std::string waiting_;
    std::vector<FilePasteStatus> statuses_;
};

} // namespace inputleap
