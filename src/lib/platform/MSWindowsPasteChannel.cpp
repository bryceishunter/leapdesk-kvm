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

#include "platform/MSWindowsPasteChannel.h"

#include <cstdint>
#include <cstring>

namespace inputleap {

namespace {

void put_u32(std::string& out, std::uint32_t value)
{
    char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                     static_cast<char>((value >> 16) & 0xff),
                     static_cast<char>((value >> 24) & 0xff)};
    out.append(bytes, 4);
}

std::uint32_t get_u32(const char* bytes)
{
    auto b = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

} // namespace

const std::string& PasteMessage::field(std::size_t i) const
{
    static const std::string none;
    return i < fields.size() ? fields[i] : none;
}

MSWindowsPasteChannel::MSWindowsPasteChannel(HANDLE pipe) :
    pipe_(pipe),
    read_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
    write_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

MSWindowsPasteChannel::~MSWindowsPasteChannel()
{
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
    }
    CloseHandle(read_event_);
    CloseHandle(write_event_);
}

std::string MSWindowsPasteChannel::encode(const PasteMessage& message)
{
    std::string body;
    put_u32(body, static_cast<std::uint32_t>(message.fields.size() + 1));
    put_u32(body, static_cast<std::uint32_t>(message.type.size()));
    body += message.type;
    for (const auto& field : message.fields) {
        put_u32(body, static_cast<std::uint32_t>(field.size()));
        body += field;
    }
    std::string framed;
    put_u32(framed, static_cast<std::uint32_t>(body.size()));
    return framed + body;
}

bool MSWindowsPasteChannel::decode(const std::string& data, PasteMessage& message)
{
    // data is the message without its frame length
    if (data.size() < 4) {
        return false;
    }
    std::uint32_t count = get_u32(data.data());
    if (count == 0 || count > 1024) {
        return false;
    }
    std::size_t pos = 4;
    std::vector<std::string> strings;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (data.size() - pos < 4) {
            return false;
        }
        std::uint32_t length = get_u32(data.data() + pos);
        pos += 4;
        if (data.size() - pos < length) {
            return false;
        }
        strings.emplace_back(data, pos, length);
        pos += length;
    }
    if (pos != data.size()) {
        return false;
    }
    message.type = strings[0];
    message.fields.assign(strings.begin() + 1, strings.end());
    return true;
}

bool MSWindowsPasteChannel::read_exactly(char* buffer, DWORD size, HANDLE stop)
{
    DWORD done = 0;
    while (done < size) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = read_event_;
        ResetEvent(read_event_);
        DWORD count = 0;
        if (!ReadFile(pipe_, buffer + done, size - done, &count, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            HANDLE events[2] = {read_event_, stop};
            DWORD waited = WaitForMultipleObjects(stop != nullptr ? 2 : 1, events, FALSE, INFINITE);
            if (waited != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &overlapped);
                GetOverlappedResult(pipe_, &overlapped, &count, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe_, &overlapped, &count, FALSE)) {
                return false;
            }
        }
        if (count == 0) {
            return false;
        }
        done += count;
    }
    return true;
}

bool MSWindowsPasteChannel::read(PasteMessage& message, HANDLE stop)
{
    char header[4];
    if (!read_exactly(header, 4, stop)) {
        return false;
    }
    std::uint32_t length = get_u32(header);
    if (length < 4 || length > kMaxMessage) {
        return false;
    }
    std::string data(length, '\0');
    return read_exactly(&data[0], length, stop) && decode(data, message);
}

bool MSWindowsPasteChannel::write(const PasteMessage& message)
{
    std::string data = encode(message);
    std::lock_guard<std::mutex> lock(write_mutex_);
    DWORD done = 0;
    while (done < data.size()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = write_event_;
        ResetEvent(write_event_);
        DWORD count = 0;
        if (!WriteFile(pipe_, data.data() + done, static_cast<DWORD>(data.size() - done), &count,
                       &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                !GetOverlappedResult(pipe_, &overlapped, &count, TRUE)) {
                return false;
            }
        }
        if (count == 0) {
            return false;
        }
        done += count;
    }
    return true;
}

} // namespace inputleap
