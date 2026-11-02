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

#include <QString>
#include <QStringList>

// What the running server or client has said about its connections, worked out from its log.
// The window only sees the lines logged since it started listening, so a state it hasn't
// seen a line for stays Unknown.
class ConnectionStatus
{
public:
    enum class Client {
        Unknown,
        Connecting,     // trying, and nothing has gone wrong yet
        Connected,
        Unreachable,    // the connection failed; failure() says why
        NotPaired,      // the server's certificate isn't one this computer trusts
        NameRefused,    // the server has no screen with this computer's name
        NameTaken       // another computer with this name is connected to the server
    };

    // takes one line of the log; returns whether anything shown changed
    bool update(const QString& line);
    // forgets everything, for when the server or client stops
    void reset();

    // as a server: whether the log has said which clients are connected, since it only says
    // when one connects or leaves
    bool knowsClients() const { return m_KnowsClients; }
    // as a server: the clients connected, in the order they connected
    const QStringList& connectedClients() const { return m_Clients; }
    bool isConnected(const QString& client) const;
    // as a server: a computer this one isn't paired with tried to connect, since the last
    // client that is paired connected
    bool unpairedAttempt() const { return m_UnpairedAttempt; }
    // as a server: a computer that tried to connect but has no screen in the layout, until it
    // connects or the layout is read again
    QString unplacedClient() const { return m_UnplacedClient; }

    // as a client
    Client client() const { return m_Client; }
    QString failure() const { return m_Failure; }

private:
    bool m_KnowsClients = false;
    QStringList m_Clients;
    bool m_UnpairedAttempt = false;
    QString m_UnplacedClient;
    Client m_Client = Client::Unknown;
    QString m_Failure;
    // why this attempt to connect was turned down, which the failure that follows doesn't say
    Client m_AttemptProblem = Client::Unknown;
};
