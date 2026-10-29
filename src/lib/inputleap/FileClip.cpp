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

#include "inputleap/FileClip.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace inputleap {

namespace {

// encoding:
//   "LDF1"
//   id                     4 byte length, then UTF-8
//   number of files        4 bytes
//   for each file:
//     name                 4 byte length, then UTF-8
//     size                 8 bytes
//     modified             8 bytes
// all integers big-endian, like the rest of the clipboard data
const char kMagic[] = "LDF1";
const std::size_t kMagicLength = 4;
const std::size_t kMaxIdLength = 64;
const std::size_t kMaxNameLength = 1024;

void write_uint32(std::string& out, std::uint32_t value)
{
    out += static_cast<char>((value >> 24) & 0xff);
    out += static_cast<char>((value >> 16) & 0xff);
    out += static_cast<char>((value >> 8) & 0xff);
    out += static_cast<char>(value & 0xff);
}

void write_uint64(std::string& out, std::uint64_t value)
{
    write_uint32(out, static_cast<std::uint32_t>(value >> 32));
    write_uint32(out, static_cast<std::uint32_t>(value));
}

void write_string(std::string& out, const std::string& text)
{
    write_uint32(out, static_cast<std::uint32_t>(text.size()));
    out += text;
}

// reads the encoding above, checking every length against what is left
class Reader {
public:
    Reader(const std::string& data, std::size_t start) : data_(data), pos_(start) { }

    bool read_uint32(std::uint32_t& value)
    {
        if (data_.size() - pos_ < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | static_cast<unsigned char>(data_[pos_++]);
        }
        return true;
    }

    bool read_uint64(std::uint64_t& value)
    {
        std::uint32_t high, low;
        if (!read_uint32(high) || !read_uint32(low)) {
            return false;
        }
        value = (static_cast<std::uint64_t>(high) << 32) | low;
        return true;
    }

    bool read_string(std::string& text, std::size_t max_length)
    {
        std::uint32_t length;
        if (!read_uint32(length) || length > max_length || data_.size() - pos_ < length) {
            return false;
        }
        text.assign(data_, pos_, length);
        pos_ += length;
        return true;
    }

    bool at_end() const { return pos_ == data_.size(); }

private:
    const std::string& data_;
    std::size_t pos_;
};

// a name that can only mean a file in the directory it is put in
bool is_plain_name(const std::string& name)
{
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string FileClip::marshall() const
{
    std::string data(kMagic, kMagicLength);
    write_string(data, id);
    write_uint32(data, static_cast<std::uint32_t>(files.size()));
    for (const auto& file : files) {
        write_string(data, file.name);
        write_uint64(data, file.size);
        write_uint64(data, file.modified);
    }
    return data;
}

bool FileClip::unmarshall(const std::string& data, FileClip& clip)
{
    if (data.compare(0, kMagicLength, kMagic, kMagicLength) != 0) {
        return false;
    }
    Reader reader(data, kMagicLength);

    std::uint32_t count;
    if (!reader.read_string(clip.id, kMaxIdLength) || clip.id.empty() ||
        !reader.read_uint32(count) || count == 0 || count > FileClip::kMaxFiles) {
        return false;
    }
    clip.files.assign(count, File());
    for (auto& file : clip.files) {
        if (!reader.read_string(file.name, kMaxNameLength) || !is_plain_name(file.name) ||
            !reader.read_uint64(file.size) || !reader.read_uint64(file.modified)) {
            return false;
        }
    }
    return reader.at_end();
}

std::string FileClip::make_id(const std::vector<std::string>& paths,
                              const std::vector<File>& files)
{
    assert(paths.size() == files.size());

    // FNV-1a.  the id tells copies apart; it isn't a secret, because the
    // source only ever sends what is on its clipboard now
    std::uint64_t hash = 14695981039346656037ull;
    auto add = [&hash](const std::string& text) {
        for (unsigned char c : text) {
            hash = (hash ^ c) * 1099511628211ull;
        }
        // separator, so that "ab" + "c" and "a" + "bc" differ
        hash = (hash ^ 0xff) * 1099511628211ull;
    };
    for (std::size_t i = 0; i < paths.size(); ++i) {
        add(paths[i]);
        add(std::to_string(files[i].size));
        add(std::to_string(files[i].modified));
    }

    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::string FileClip::transfer_name(const std::string& request, std::size_t index) const
{
    return transfer_prefix(request, index) + files.at(index).name;
}

std::string FileClip::transfer_prefix(const std::string& request, std::size_t index)
{
    return "leapdesk-" + request + "-" + std::to_string(index + 1) + "-";
}

std::string file_paste_progress(std::uint64_t sent, std::uint64_t total)
{
    return "progress " + std::to_string(sent) + " " + std::to_string(total);
}

bool parse_file_paste_progress(const std::string& detail, std::uint64_t& sent,
                               std::uint64_t& total)
{
    const std::string prefix = "progress ";
    if (detail.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    char* end = nullptr;
    sent = std::strtoull(detail.c_str() + prefix.size(), &end, 10);
    if (end == nullptr || *end != ' ') {
        return false;
    }
    total = std::strtoull(end + 1, &end, 10);
    return end != nullptr && *end == '\0';
}

bool is_valid_paste_request(const std::string& request)
{
    if (request.size() < 8 || request.size() > 64) {
        return false;
    }
    for (char c : request) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_valid_paste_address(const std::string& address)
{
    // an IPv4 or IPv6 address or a machine name.  a leading '-' could be read
    // as an option by the program that sends the files
    if (address.empty() || address.size() > 253 || address[0] == '-') {
        return false;
    }
    for (char c : address) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == ':' || c == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace inputleap
