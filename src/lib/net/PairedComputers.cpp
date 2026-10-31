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

#include "net/PairedComputers.h"

#include "common/DataDirectories.h"
#include "io/StreamFilter.h"
#include "net/FingerprintDatabase.h"
#include "net/IDataSocket.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace inputleap {

void PairedNames::read(const fs::path& path)
{
    std::ifstream file;
    open_utf8_path(file, path, std::ios_base::in);
    read_stream(file);
}

bool PairedNames::write(const fs::path& path) const
{
    std::ostringstream text;
    write_stream(text);
    return replace_file(path, text.str());
}

void PairedNames::read_stream(std::istream& stream)
{
    std::string line;
    while (stream.good() && std::getline(stream, line)) {
        auto space = line.find(' ');
        if (space == std::string::npos || space + 1 == line.size()) {
            continue;
        }
        auto fingerprint = FingerprintDatabase::parse_db_line(line.substr(0, space));
        if (fingerprint.valid()) {
            set_name(fingerprint, line.substr(space + 1));
        }
    }
}

void PairedNames::write_stream(std::ostream& stream) const
{
    for (const auto& entry : names_) {
        stream << FingerprintDatabase::to_db_line(entry.first) << " " << entry.second << "\n";
    }
}

std::string PairedNames::name_of(const FingerprintData& fingerprint) const
{
    for (const auto& entry : names_) {
        if (entry.first == fingerprint) {
            return entry.second;
        }
    }
    return {};
}

void PairedNames::set_name(const FingerprintData& fingerprint, const std::string& name)
{
    forget(fingerprint);
    names_.emplace_back(fingerprint, name);
}

void PairedNames::forget(const FingerprintData& fingerprint)
{
    names_.erase(std::remove_if(names_.begin(), names_.end(),
                                [&](const auto& entry) { return entry.first == fingerprint; }),
                 names_.end());
}

FingerprintData peer_fingerprint(IStream* stream)
{
    while (auto* filter = dynamic_cast<StreamFilter*>(stream)) {
        stream = filter->getStream();
    }
    auto* socket = dynamic_cast<IDataSocket*>(stream);
    return socket != nullptr ? socket->peer_fingerprint() : FingerprintData{};
}

void remember_computer_name(const FingerprintData& fingerprint, const std::string& name)
{
    const auto path = DataDirectories::paired_names_path();
    PairedNames names;
    names.read(path);
    if (names.name_of(fingerprint) != name) {
        names.set_name(fingerprint, name);
        names.write(path);
    }
}

bool forget_computer(const fs::path& trust_path, const FingerprintData& fingerprint)
{
    FingerprintDatabase db;
    if (!db.read(trust_path)) {
        return false;
    }
    if (db.is_trusted(fingerprint)) {
        db.remove_trusted(fingerprint);
        if (!db.write(trust_path)) {
            return false;
        }
    }

    const auto names_path = DataDirectories::paired_names_path();
    PairedNames names;
    names.read(names_path);
    if (!names.name_of(fingerprint).empty()) {
        names.forget(fingerprint);
        names.write(names_path);
    }
    return true;
}

} // namespace inputleap
