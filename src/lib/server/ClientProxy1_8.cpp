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

#include "server/ClientProxy1_8.h"

#include "server/IClientConnection.h"
#include "server/Server.h"
#include "inputleap/protocol_types.h"

#include <cstring>

namespace inputleap {

ClientProxy1_8::ClientProxy1_8(const std::string& name, std::unique_ptr<IClientConnection> backend,
                               Server* server, IEventQueue* events) :
    ClientProxy1_7(name, std::move(backend), server, events)
{
}

ClientProxy1_8::~ClientProxy1_8() = default;

bool ClientProxy1_8::parseMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgCUnpair, 4) == 0) {
        getServer()->unpaired_by_client(this);
        return true;
    }
    return ClientProxy1_7::parseMessage(code);
}

} // namespace inputleap
