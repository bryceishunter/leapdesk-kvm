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

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace inputleap {

//! A message between the service and the paste helper running as the user
struct PasteMessage {
    std::string type;
    std::vector<std::string> fields;

    PasteMessage() = default;
    PasteMessage(std::string type_, std::vector<std::string> fields_ = {}) :
        type(std::move(type_)), fields(std::move(fields_)) {}

    //! Field \p i, or empty if there's none
    const std::string& field(std::size_t i) const;
};

//! Messages over a named pipe opened for overlapped I/O
/*!
Each message is a 32-bit length and then the message: a 32-bit count of
strings, the type first, then each string as a 32-bit length and its bytes.
Reading can be stopped from another thread; writing is safe from any thread.
*/
class MSWindowsPasteChannel {
public:
    //! The largest message read; an offer holds a whole clipboard
    static const std::size_t kMaxMessage = 64u * 1024 * 1024;

    //! Takes \p pipe, opened with FILE_FLAG_OVERLAPPED, and closes it later
    explicit MSWindowsPasteChannel(HANDLE pipe);
    ~MSWindowsPasteChannel();

    MSWindowsPasteChannel(const MSWindowsPasteChannel&) = delete;
    MSWindowsPasteChannel& operator=(const MSWindowsPasteChannel&) = delete;

    //! Reads the next message.  False when the other end has gone, the data
    //! is broken, or \p stop (an event, may be null) is set.
    bool read(PasteMessage& message, HANDLE stop);

    //! Writes \p message; false if the other end has gone
    bool write(const PasteMessage& message);

    HANDLE handle() const { return pipe_; }

    static std::string encode(const PasteMessage& message);
    static bool decode(const std::string& data, PasteMessage& message);

private:
    bool read_exactly(char* buffer, DWORD size, HANDLE stop);

    HANDLE pipe_;
    HANDLE read_event_;
    HANDLE write_event_;
    std::mutex write_mutex_;
};

} // namespace inputleap
