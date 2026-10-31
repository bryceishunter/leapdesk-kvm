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

#include "io/filesystem.h"
#include <string>

namespace inputleap {

// Notices that a file was written, for whoever checks on it every so often.
class FileChangeWatch {
public:
    explicit FileChangeWatch(const fs::path& path);

    // Whether the file changed since this last said so. It says so at the first check where the
    // file is the same as at the check before, so that a file still being written is not read;
    // a file that has gone waits until it's back.
    bool changed();

private:
    fs::path path_;
    std::string stamp_;
    bool changing_ = false;
};

} // namespace inputleap
