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

#include "platform/MSWindowsFileFetch.h"

#include "platform/MSWindowsFilePaste.h"
#include "platform/MSWindowsSessionUser.h"
#include "common/win32/encoding_utilities.h"
#include "base/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>

namespace inputleap {

namespace {

std::wstring widen(const std::string& text)
{
    return utf8_to_win_char(text).data();
}

std::string narrow(const std::wstring& text)
{
    return win_wchar_to_utf8(text.c_str());
}

} // namespace

MSWindowsFileFetch::MSWindowsFileFetch(Settings settings) :
    settings_(std::move(settings))
{
}

bool MSWindowsFileFetch::fetch(const FileClip& clip, const std::string& requester,
                               std::vector<std::wstring>& paths, std::string& failure)
{
    LOG_NOTE("pasting %zu file(s) copied on another screen, for %s", clip.files.size(),
             requester.c_str());

    std::string request = MSWindowsFilePaste::new_request();
    failure.clear();
    paths.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            failure = "Leapdesk is stopping";
        }
        else if (!connected_) {
            failure = "the other screens aren't connected";
        }
        else {
            waiting_ = request;
            statuses_.clear();
            cancelled_ = false;
        }
    }

    Progress progress;
    progress.request = request;
    progress.files = clip.files.size();
    progress.name = clip.files.empty() ? std::string() : clip.files.front().name;
    for (const auto& file : clip.files) {
        progress.total += file.size;
    }

    if (failure.empty()) {
        std::string address = settings_.own_address();
        if (address.empty()) {
            failure = "this machine has no Tailscale address";
        }
        else {
            tell(progress);
            settings_.request(FilePasteRequest{request, clip.id, address});
            if (wait_for_files(progress, failure)) {
                collect(clip, request, paths, failure);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting_.clear();
        statuses_.clear();
    }
    Progress end = progress;
    end.stage = failure.empty() ? Progress::Stage::DONE : Progress::Stage::FAILED;
    end.detail = failure;
    tell(end);
    if (!failure.empty()) {
        LOG_NOTE("couldn't paste the files: %s", failure.c_str());
        paths.clear();
        return false;
    }
    LOG_NOTE("pasted %zu file(s)", paths.size());
    return true;
}

void MSWindowsFileFetch::tell(const Progress& progress)
{
    if (settings_.progress) {
        settings_.progress(progress);
    }
}

bool MSWindowsFileFetch::cancel(const std::string& request)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_.empty() || waiting_ != request || cancelled_) {
        return false;
    }
    cancelled_ = true;
    changed_.notify_all();
    return true;
}

void MSWindowsFileFetch::status(const FilePasteStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_.empty() || status.request != waiting_) {
        // the service's own fetch and the helper's both hear every status
        std::uint64_t sent = 0;
        std::uint64_t total = 0;
        LOG(parse_file_paste_progress(status.detail, sent, total) ? kDEBUG2 : kDEBUG,
            "ignored file paste status for request %s", status.request.c_str());
        return;
    }
    statuses_.push_back(status);
    changed_.notify_all();
}

void MSWindowsFileFetch::set_connected(bool connected)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = connected;
    changed_.notify_all();
}

void MSWindowsFileFetch::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    changed_.notify_all();
}

bool MSWindowsFileFetch::wait_for_files(const Progress& start, std::string& failure)
{
    const std::string& request = start.request;
    Progress progress = start;
    std::unique_lock<std::mutex> lock(mutex_);
    auto heard = [this]() {
        return stopping_ || !connected_ || cancelled_ || !statuses_.empty();
    };
    auto deadline = std::chrono::steady_clock::now() + settings_.answer_timeout;
    bool sending = false;
    // a sender that reports progress does so several times a second; one
    // that stops for long has stopped
    bool reports_progress = false;
    auto last_heard = std::chrono::steady_clock::now();

    for (;;) {
        auto until = deadline;
        if (reports_progress) {
            until = (std::min)(until, last_heard + settings_.stall_timeout);
        }
        if (!changed_.wait_until(lock, until, heard)) {
            if (reports_progress && std::chrono::steady_clock::now() < deadline) {
                failure = "the transfer stopped making progress";
            }
            else {
                failure = sending ? "the files took too long to arrive" :
                                    "the screen that copied the files didn't answer";
            }
            return false;
        }
        if (stopping_) {
            failure = "Leapdesk is stopping";
            return false;
        }
        if (cancelled_) {
            failure = kCancelled;
            return false;
        }
        if (!connected_) {
            failure = "the connection to the other screens was lost";
            return false;
        }
        std::vector<FilePasteStatus> statuses;
        statuses.swap(statuses_);
        last_heard = std::chrono::steady_clock::now();
        for (const auto& status : statuses) {
            switch (status.state) {
            case FilePasteState::SENDING: {
                progress.stage = Progress::Stage::SENDING;
                std::uint64_t sent = 0;
                std::uint64_t total = 0;
                if (parse_file_paste_progress(status.detail, sent, total)) {
                    reports_progress = true;
                    progress.counting = true;
                    progress.sent = sent;
                    progress.total = total;
                }
                else {
                    LOG_INFO("paste %s: %s", request.c_str(), status.detail.c_str());
                }
                if (!sending) {
                    sending = true;
                    deadline = std::chrono::steady_clock::now() + settings_.transfer_timeout;
                }
                lock.unlock();
                tell(progress);
                lock.lock();
                break;
            }
            case FilePasteState::SENT:
                return true;
            case FilePasteState::FAILED:
                failure = status.detail;
                return false;
            }
        }
    }
}

bool MSWindowsFileFetch::collect(const FileClip& clip, const std::string& request,
                                 std::vector<std::wstring>& paths, std::string& failure)
{
    // under the service this runs as SYSTEM, which can ask for the user's
    // folder by their token
    std::wstring received = settings_.received_folder();
    if (received.empty()) {
        failure = "the folder Taildrop saves files in wasn't found";
        return false;
    }

    // the rest happens in the user's folders, so it is done as the user (a
    // no-op when this already runs as the user)
    ImpersonateSessionUser as_user;
    if (as_user.failed()) {
        failure = "couldn't act as the signed-in user";
        return false;
    }

    // each file gets a folder of its own, so that files with the same name
    // from different folders don't collide
    std::wstring folder = settings_.staging_folder + L"\\" + widen(request);
    CreateDirectoryW(settings_.staging_folder.c_str(), nullptr);
    if (!CreateDirectoryW(folder.c_str(), nullptr)) {
        failure = "couldn't create " + narrow(folder);
        return false;
    }

    for (std::size_t i = 0; i < clip.files.size(); ++i) {
        std::string name = clip.transfer_name(request, i);
        std::string prefix = FileClip::transfer_prefix(request, i);
        auto deadline = std::chrono::steady_clock::now() + settings_.arrival_timeout;
        std::wstring from;
        while ((from = MSWindowsFilePaste::find_received(received, prefix, name)).empty()) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cancelled_) {
                failure = kCancelled;
                return false;
            }
            if (stopping_ || std::chrono::steady_clock::now() >= deadline) {
                failure = name + " didn't arrive in " + narrow(received);
                return false;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(200));
        }

        std::wstring file_folder = folder + L"\\" + std::to_wstring(i + 1);
        std::wstring to = file_folder + L"\\" + widen(clip.files[i].name);
        if (!CreateDirectoryW(file_folder.c_str(), nullptr)) {
            failure = "couldn't create " + narrow(file_folder);
            return false;
        }

        // Taildrop can still have the file open for a moment after it's sent,
        // and so can a virus scan of the new file.  failing then would make
        // the application ask again, which sends the files again
        auto released = std::chrono::steady_clock::now() + settings_.arrival_timeout;
        auto waited = std::chrono::steady_clock::now();
        while (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
            DWORD error = GetLastError();
            bool busy = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
                        error == ERROR_ACCESS_DENIED;
            std::unique_lock<std::mutex> lock(mutex_);
            if (cancelled_) {
                failure = kCancelled;
                return false;
            }
            if (!busy || stopping_ || std::chrono::steady_clock::now() >= released) {
                failure = "couldn't move " + narrow(from) + ": " + std::to_string(error);
                return false;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(100));
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waited).count();
        if (ms >= 100) {
            LOG_DEBUG("waited %lld ms for %s to be closed before moving it",
                      static_cast<long long>(ms), narrow(from).c_str());
        }
        paths.push_back(to);
    }
    return true;
}

} // namespace inputleap
