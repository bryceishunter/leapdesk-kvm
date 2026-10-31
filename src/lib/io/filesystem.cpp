/*
    InputLeap -- mouse and keyboard sharing utility
    Copyright (C) InputLeap contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if INPUTLEAP_USE_GULRAK_FILESYSTEM
// this header must come first so that it picks up the filesystem implementation
#include <ghc/fs_impl.hpp>
#endif

#include "filesystem.h"
#if SYSAPI_WIN32
#include "common/win32/encoding_utilities.h"
#endif
#include <chrono>
#include <fstream>
#include <system_error>
#include <thread>

namespace inputleap {

namespace {

template<class Stream>
void open_utf8_path_impl(Stream& stream, const fs::path& path, std::ios_base::openmode mode)
{
#if SYSAPI_WIN32
    // on Windows we need to use a non-standard constructor from wchar_t* string
    // which fs::path::native() returns
    stream.open(path.native().c_str(), mode);
#else
    stream.open(path.native().c_str(), mode);
#endif
}

} // namespace

void open_utf8_path(std::ifstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

void open_utf8_path(std::ofstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

void open_utf8_path(std::fstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

std::FILE* fopen_utf8_path(const fs::path& path, const std::string& mode)
{
#if SYSAPI_WIN32
    auto wchar_mode = utf8_to_win_char(mode);
    return _wfopen(path.native().c_str(),
                   reinterpret_cast<wchar_t*>(wchar_mode.data()));
#else
    return std::fopen(path.native().c_str(), mode.c_str());
#endif
}

bool replace_file(const fs::path& path, const std::string& contents)
{
    fs::path temporary = path;
    temporary += ".new";

    std::error_code error;
    {
        std::ofstream file;
        open_utf8_path(file, temporary, std::ios_base::out);
        file << contents;
        file.close();
        if (file.fail()) {
            fs::remove(temporary, error);
            return false;
        }
    }

    // on Windows a file can't be replaced while it's open, but it's only open for a moment
    for (int tries = 0; tries < 20; ++tries) {
        fs::rename(temporary, path, error);
        if (!error) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    fs::remove(temporary, error);
    return false;
}

} // namespace inputleap
