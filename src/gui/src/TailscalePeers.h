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

#include <QByteArray>
#include <QList>
#include <QString>

// One device on this computer's tailnet, from `tailscale status --json`.
struct TailscalePeer {
    enum class Path {
        Offline,    // Tailscale can't see it
        Idle,       // online, but nothing is being sent, so there is no path to report
        Direct,     // traffic goes straight to it, at direct_address
        Relayed     // traffic goes through a Tailscale relay, named relay
    };

    QString name;           // the first part of its MagicDNS name
    QString address;        // its Tailscale IPv4 address
    Path path = Path::Offline;
    QString direct_address;
    QString relay;
};

// The devices in the JSON, online ones first and then by name; none if it can't be read.
QList<TailscalePeer> parse_tailscale_status(const QByteArray& json);

// The device a server address such as 100.111.152.25:24800 or desktop-gp522k1 points at,
// or nullptr if it isn't on the tailnet.
const TailscalePeer* find_tailscale_peer(const QList<TailscalePeer>& peers, const QString& address);
