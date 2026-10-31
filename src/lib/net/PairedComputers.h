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

#include "FingerprintData.h"
#include "io/filesystem.h"
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace inputleap {

class IStream;

// What people call the computers this one is paired with, since the fingerprint databases hold
// only certificates. The names are for showing to people: the databases alone decide who is
// trusted.
class PairedNames {
public:
    void read(const fs::path& path);
    bool write(const fs::path& path) const;

    void read_stream(std::istream& stream);
    void write_stream(std::ostream& stream) const;

    // the computer's name, or nothing if none is known
    std::string name_of(const FingerprintData& fingerprint) const;
    void set_name(const FingerprintData& fingerprint, const std::string& name);
    void forget(const FingerprintData& fingerprint);

private:
    std::vector<std::pair<FingerprintData, std::string>> names_;
};

// The fingerprint of the certificate the computer at the other end of a stream proved it holds.
// Invalid if no certificate was checked, so nothing that stream says speaks for a paired computer.
FingerprintData peer_fingerprint(IStream* stream);

// Records what a paired computer is called, unless that's already known.
void remember_computer_name(const FingerprintData& fingerprint, const std::string& name);

// Stops trusting a computer: takes it out of the database at trust_path and forgets its name.
// Returns false if the database couldn't be changed, and then the computer is still trusted.
bool forget_computer(const fs::path& trust_path, const FingerprintData& fingerprint);

} // namespace inputleap
