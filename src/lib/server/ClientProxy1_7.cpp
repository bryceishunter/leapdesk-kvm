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

#include "server/ClientProxy1_7.h"

#include "server/IClientConnection.h"
#include "server/Server.h"
#include "inputleap/FileClip.h"
#include "inputleap/ProtocolUtil.h"
#include "inputleap/protocol_types.h"
#include "base/Log.h"

#include <cstring>

namespace inputleap {

ClientProxy1_7::ClientProxy1_7(const std::string& name, std::unique_ptr<IClientConnection> backend,
                               Server* server, IEventQueue* events) :
    ClientProxy1_6(name, std::move(backend), server, events)
{
}

ClientProxy1_7::~ClientProxy1_7() = default;

void ClientProxy1_7::send_files(const FilePasteRequest& request)
{
    get_conn().send_file_send_1_7(request);
}

void ClientProxy1_7::file_paste_status(const FilePasteStatus& status)
{
    get_conn().send_file_status_1_7(status);
}

bool ClientProxy1_7::parseMessage(const std::uint8_t* code)
{
    if (memcmp(code, kMsgQFilePaste, 4) == 0) {
        return recv_file_paste();
    }
    if (memcmp(code, kMsgDFileStatus, 4) == 0) {
        return recv_file_status();
    }
    return ClientProxy1_6::parseMessage(code);
}

bool ClientProxy1_7::recv_file_paste()
{
    FilePasteRequest request;
    if (!ProtocolUtil::readf(getStream(), kMsgQFilePaste + 4, &request.request, &request.file_id,
                             &request.address)) {
        return false;
    }
    LOG_DEBUG("recv file paste request=%s file_id=%s address=%s from \"%s\"",
              request.request.c_str(), request.file_id.c_str(), request.address.c_str(),
              getName().c_str());
    getServer()->file_paste_requested(this, request);
    return true;
}

bool ClientProxy1_7::recv_file_status()
{
    FilePasteStatus status;
    std::uint8_t state;
    if (!ProtocolUtil::readf(getStream(), kMsgDFileStatus + 4, &status.request, &state,
                             &status.detail)) {
        return false;
    }
    if (state < static_cast<std::uint8_t>(FilePasteState::SENDING) ||
        state > static_cast<std::uint8_t>(FilePasteState::FAILED)) {
        LOG_ERR("unknown file paste state %d from \"%s\"", state, getName().c_str());
        return false;
    }
    status.state = static_cast<FilePasteState>(state);
    // progress comes a few times a second
    std::uint64_t sent = 0;
    std::uint64_t total = 0;
    LOG(parse_file_paste_progress(status.detail, sent, total) ? kDEBUG2 : kDEBUG,
        "recv file status request=%s state=%d detail=%s from \"%s\"", status.request.c_str(),
        state, status.detail.c_str(), getName().c_str());
    getServer()->file_paste_status(this, status);
    return true;
}

} // namespace inputleap
