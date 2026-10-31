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

#include "../src/ConnectionStatus.h"

#include <gtest/gtest.h>

#include <initializer_list>

namespace {

// lines as the service's log has them
ConnectionStatus after(std::initializer_list<const char*> lines)
{
    ConnectionStatus status;
    for (const char* line : lines)
        status.update(QString("[2026-10-30T15:52:56] ") + line);
    return status;
}

} // namespace

TEST(ConnectionStatusTests, followsTheClientsOfAServer)
{
    auto status = after({
        "NOTE: started server (IPv4/IPv6), waiting for clients",
        "NOTE: accepted client connection",
        "NOTE: client \"surfacestudio\" has connected",
        "NOTE: client \"laptop\" has connected",
    });
    EXPECT_EQ(status.connectedClients(), QStringList({"surfacestudio", "laptop"}));
    EXPECT_TRUE(status.isConnected("SurfaceStudio"));

    status.update("NOTE: client \"surfacestudio\" has disconnected");
    EXPECT_EQ(status.connectedClients(), QStringList({"laptop"}));
    status.update("NOTE: client \"laptop\" is dead");
    EXPECT_TRUE(status.connectedClients().isEmpty());
}

TEST(ConnectionStatusTests, onlyAServerStartingSaysWhoIsntConnected)
{
    // a window opened after the server started hears of a client only when it comes or goes
    auto status = after({"NOTE: client \"laptop\" has connected"});
    EXPECT_FALSE(status.knowsClients());
    EXPECT_TRUE(status.isConnected("laptop"));

    status.update("NOTE: started server (IPv4/IPv6), waiting for clients");
    EXPECT_TRUE(status.knowsClients());
}

TEST(ConnectionStatusTests, aServerStartingAgainHasNoClients)
{
    auto status = after({
        "NOTE: client \"surfacestudio\" has connected",
        "NOTE: started server (IPv4/IPv6), waiting for clients",
    });
    EXPECT_TRUE(status.connectedClients().isEmpty());
}

TEST(ConnectionStatusTests, aServerNoticesAComputerItIsntPairedWith)
{
    auto status = after({
        "NOTE: accepted client connection",
        "NOTE: Fingerprint does not match trusted fingerprint",
        "ERROR: failed to verify client certificate fingerprint",
    });
    EXPECT_TRUE(status.unpairedAttempt());

    status.update("NOTE: client \"surfacestudio\" has connected");
    EXPECT_FALSE(status.unpairedAttempt());
}

TEST(ConnectionStatusTests, aClientConnects)
{
    auto status = after({
        "NOTE: started client",
        "NOTE: connecting to '100.111.152.25': 100.111.152.25:24800",
    });
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Connecting);

    EXPECT_TRUE(status.update("NOTE: connected to server"));
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Connected);

    status.update("NOTE: disconnected from server");
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Connecting);
}

TEST(ConnectionStatusTests, aClientSaysWhyItCantConnect)
{
    auto status = after({
        "NOTE: connecting to '100.111.152.25': 100.111.152.25:24800",
        "WARNING: failed to connect to server: Timed out",
    });
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Unreachable);
    EXPECT_EQ(status.failure(), "Timed out");

    // trying again doesn't hide why the last try failed
    EXPECT_FALSE(status.update("NOTE: connecting to '100.111.152.25': 100.111.152.25:24800"));
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Unreachable);
}

TEST(ConnectionStatusTests, aClientThatIsntPairedSaysSo)
{
    auto status = after({
        "NOTE: connecting to '100.111.152.25': 100.111.152.25:24800",
        "NOTE: Fingerprint does not match trusted fingerprint",
        "ERROR: failed to verify server certificate fingerprint",
        "WARNING: failed to connect to server: failed to verify server certificate fingerprint",
    });
    EXPECT_EQ(status.client(), ConnectionStatus::Client::NotPaired);

    // and once it's paired, a different failure is told as itself
    status.update("NOTE: connecting to '100.111.152.25': 100.111.152.25:24800");
    status.update("WARNING: failed to connect to server: Connection refused");
    EXPECT_EQ(status.client(), ConnectionStatus::Client::Unreachable);
    EXPECT_EQ(status.failure(), "Connection refused");
}

TEST(ConnectionStatusTests, aClientTheServerDoesntKnow)
{
    auto status = after({
        "NOTE: connecting to '100.111.152.25': 100.111.152.25:24800",
        "ERROR: server refused client with name \"surfacestudio\"",
        "WARNING: failed to connect to server: server refused client with our name",
    });
    EXPECT_EQ(status.client(), ConnectionStatus::Client::NameRefused);

    status = after({
        "ERROR: server already has a connected client with name \"surfacestudio\"",
        "WARNING: failed to connect to server: server already has a connected client with our name",
    });
    EXPECT_EQ(status.client(), ConnectionStatus::Client::NameTaken);
}
