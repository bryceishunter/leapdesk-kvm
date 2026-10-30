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

#include "../src/TailscalePeers.h"

#include <gtest/gtest.h>

namespace {

// the fields that matter, as `tailscale status --json` gives them
const char kStatus[] = R"({
  "Self": { "HostName": "me" },
  "Peer": {
    "key1": { "HostName": "phone", "DNSName": "phone.tailcba81.ts.net.",
              "TailscaleIPs": ["100.81.175.81", "fd7a:115c:a1e0::1"],
              "Online": false, "Active": false, "CurAddr": "", "Relay": "tor" },
    "key2": { "HostName": "Surface Studio", "DNSName": "surfacestudio.tailcba81.ts.net.",
              "TailscaleIPs": ["100.81.171.127", "fd7a:115c:a1e0::2"],
              "Online": true, "Active": true, "CurAddr": "192.168.0.45:41641", "Relay": "tor" },
    "key3": { "HostName": "laptop", "DNSName": "laptop.tailcba81.ts.net.",
              "TailscaleIPs": ["fd7a:115c:a1e0::3", "100.92.14.33"],
              "Online": true, "Active": true, "CurAddr": "", "Relay": "ord" },
    "key4": { "HostName": "desk", "DNSName": "desk.tailcba81.ts.net.",
              "TailscaleIPs": ["100.111.152.25"],
              "Online": true, "Active": false, "CurAddr": "", "Relay": "tor" },
    "key5": { "HostName": "v6only", "DNSName": "v6only.tailcba81.ts.net.",
              "TailscaleIPs": ["fd7a:115c:a1e0::5"], "Online": true }
  }
})";

} // namespace

TEST(TailscalePeersTests, readsOnlineDevicesFirstThenByName)
{
    auto peers = parse_tailscale_status(kStatus);

    ASSERT_EQ(peers.size(), 4);
    EXPECT_EQ(peers[0].name.toStdString(), "desk");
    EXPECT_EQ(peers[1].name.toStdString(), "laptop");
    EXPECT_EQ(peers[2].name.toStdString(), "surfacestudio");
    EXPECT_EQ(peers[3].name.toStdString(), "phone");
}

TEST(TailscalePeersTests, usesTheIpv4Address)
{
    auto peers = parse_tailscale_status(kStatus);
    EXPECT_EQ(peers[1].address.toStdString(), "100.92.14.33");
}

TEST(TailscalePeersTests, tellsThePathApart)
{
    auto peers = parse_tailscale_status(kStatus);

    EXPECT_EQ(peers[2].path, TailscalePeer::Path::Direct);
    EXPECT_EQ(peers[2].direct_address.toStdString(), "192.168.0.45:41641");

    EXPECT_EQ(peers[1].path, TailscalePeer::Path::Relayed);
    EXPECT_EQ(peers[1].relay.toStdString(), "ord");

    // names a relay it would use, but isn't using one
    EXPECT_EQ(peers[0].path, TailscalePeer::Path::Idle);

    EXPECT_EQ(peers[3].path, TailscalePeer::Path::Offline);
}

TEST(TailscalePeersTests, somethingElseGivesNoDevices)
{
    EXPECT_TRUE(parse_tailscale_status("").isEmpty());
    EXPECT_TRUE(parse_tailscale_status("not json").isEmpty());
    EXPECT_TRUE(parse_tailscale_status("{}").isEmpty());
}

TEST(TailscalePeersTests, findsADeviceByAddressOrName)
{
    auto peers = parse_tailscale_status(kStatus);
    auto name_of = [&](const char* address) {
        const TailscalePeer* peer = find_tailscale_peer(peers, address);
        return peer ? peer->name.toStdString() : std::string("none");
    };

    EXPECT_EQ(name_of("100.111.152.25"), "desk");
    EXPECT_EQ(name_of("100.111.152.25:24800"), "desk");
    EXPECT_EQ(name_of("desk"), "desk");
    EXPECT_EQ(name_of("DESK:24800"), "desk");
    EXPECT_EQ(name_of("surfacestudio.tailcba81.ts.net"), "surfacestudio");
    EXPECT_EQ(name_of("192.168.0.9"), "none");
    EXPECT_EQ(name_of(""), "none");
}
