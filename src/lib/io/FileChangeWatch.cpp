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

#include "io/FileChangeWatch.h"

#include <system_error>

namespace inputleap {

namespace {

const char kMissing[] = "missing";

// size and modification time, enough to tell that a file was written
std::string file_stamp(const fs::path& path)
{
    std::error_code error;
    auto size = fs::file_size(path, error);
    if (error) {
        return kMissing;
    }
    auto time = fs::last_write_time(path, error);
    if (error) {
        return kMissing;
    }
    return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

} // namespace

FileChangeWatch::FileChangeWatch(const fs::path& path) :
    path_(path),
    stamp_(file_stamp(path))
{
}

bool FileChangeWatch::changed()
{
    std::string stamp = file_stamp(path_);
    if (stamp != stamp_) {
        stamp_ = stamp;
        changing_ = true;
        return false;
    }
    if (changing_ && stamp != kMissing) {
        changing_ = false;
        return true;
    }
    return false;
}

} // namespace inputleap
