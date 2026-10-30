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

#include "TailscalePeers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

QList<TailscalePeer> parse_tailscale_status(const QByteArray& json)
{
    QList<TailscalePeer> peers;

    const QJsonObject all = QJsonDocument::fromJson(json).object().value("Peer").toObject();
    for (const QJsonValue& value : all) {
        const QJsonObject peer_json = value.toObject();

        TailscalePeer peer;
        const QString dns_name = peer_json.value("DNSName").toString();
        peer.name = dns_name.isEmpty() ? peer_json.value("HostName").toString()
                                       : dns_name.section('.', 0, 0);
        for (const QJsonValue& address : peer_json.value("TailscaleIPs").toArray()) {
            if (address.toString().contains('.')) {
                peer.address = address.toString();
                break;
            }
        }
        if (peer.address.isEmpty())
            continue;

        // an idle peer has no current address, and still names the relay it would use
        peer.direct_address = peer_json.value("CurAddr").toString();
        peer.relay = peer_json.value("Relay").toString();
        if (!peer_json.value("Online").toBool())
            peer.path = TailscalePeer::Path::Offline;
        else if (!peer.direct_address.isEmpty())
            peer.path = TailscalePeer::Path::Direct;
        else if (peer_json.value("Active").toBool())
            peer.path = TailscalePeer::Path::Relayed;
        else
            peer.path = TailscalePeer::Path::Idle;

        peers.append(peer);
    }

    std::sort(peers.begin(), peers.end(), [](const TailscalePeer& a, const TailscalePeer& b) {
        bool a_online = a.path != TailscalePeer::Path::Offline;
        bool b_online = b.path != TailscalePeer::Path::Offline;
        if (a_online != b_online)
            return a_online;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return peers;
}

const TailscalePeer* find_tailscale_peer(const QList<TailscalePeer>& peers, const QString& address)
{
    QString host = address.trimmed();
    if (host.count(':') == 1)
        host = host.section(':', 0, 0);
    if (host.isEmpty())
        return nullptr;
    const QString first_part = host.section('.', 0, 0);

    for (const TailscalePeer& peer : peers) {
        if (peer.address == host || peer.name.compare(first_part, Qt::CaseInsensitive) == 0)
            return &peer;
    }
    return nullptr;
}
