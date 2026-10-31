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

#include "ConnectionStatus.h"

#include <QRegularExpression>

namespace {

// the name in a line such as: client "surfacestudio" has connected
QString quotedName(const QString& line, const QString& after)
{
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]*)\""));
    const int start = line.indexOf(after);
    if (start < 0)
        return QString();
    const auto match = quoted.match(line, start + after.size() - 1);
    return match.hasMatch() ? match.captured(1) : QString();
}

} // namespace

bool ConnectionStatus::update(const QString& line)
{
    const bool old_knows = m_KnowsClients;
    const QStringList old_clients = m_Clients;
    const bool old_attempt = m_UnpairedAttempt;
    const Client old_client = m_Client;
    const QString old_failure = m_Failure;

    // the server's side
    if (line.contains(QLatin1String("started server"))) {
        m_KnowsClients = true;
        m_Clients.clear();
        m_UnpairedAttempt = false;
    } else if (line.contains(QLatin1String("\" has connected"))) {
        const QString name = quotedName(line, QStringLiteral("client \""));
        if (!name.isEmpty() && !isConnected(name))
            m_Clients.append(name);
        m_UnpairedAttempt = false;
    } else if (line.contains(QLatin1String("\" has disconnected")) || line.contains(QLatin1String("\" is dead")) ||
               line.contains(QLatin1String("\" was unpaired")) || line.contains(QLatin1String("disconnecting client \"")) ||
               line.contains(QLatin1String("forced disconnection of client \""))) {
        const QString name = quotedName(line, QStringLiteral("client \""));
        m_Clients.removeIf([&](const QString& client) { return client.compare(name, Qt::CaseInsensitive) == 0; });
    }

    // either side: the certificate at the other end isn't trusted
    if (line.contains(QLatin1String("Fingerprint does not match trusted fingerprint"))) {
        m_UnpairedAttempt = true;
        m_AttemptProblem = m_Client = Client::NotPaired;
    }

    // the client's side
    static const QLatin1String failed("failed to connect to server: ");
    if (line.contains(QLatin1String("connecting to '"))) {
        // what the last attempt found stays shown until this one gets somewhere
        m_AttemptProblem = Client::Unknown;
        if (m_Client == Client::Unknown || m_Client == Client::Connected)
            m_Client = Client::Connecting;
    } else if (line.contains(QLatin1String("connected to server"))) {
        m_Client = Client::Connected;
        m_Failure.clear();
    } else if (line.contains(failed)) {
        if (m_AttemptProblem != Client::Unknown) {
            m_Client = m_AttemptProblem;
        } else {
            m_Client = Client::Unreachable;
            m_Failure = line.mid(line.indexOf(failed) + failed.size()).trimmed();
        }
    } else if (line.contains(QLatin1String("server refused client with name"))) {
        m_AttemptProblem = m_Client = Client::NameRefused;
    } else if (line.contains(QLatin1String("server already has a connected client with name"))) {
        m_AttemptProblem = m_Client = Client::NameTaken;
    } else if (line.contains(QLatin1String("disconnected from server")) || line.contains(QLatin1String("server is dead"))) {
        if (m_Client == Client::Connected)
            m_Client = Client::Connecting;
    } else if (line.contains(QLatin1String("started client"))) {
        m_Client = Client::Connecting;
        m_Failure.clear();
        m_AttemptProblem = Client::Unknown;
    }

    return m_KnowsClients != old_knows || m_Clients != old_clients || m_UnpairedAttempt != old_attempt ||
           m_Client != old_client || m_Failure != old_failure;
}

void ConnectionStatus::reset()
{
    *this = ConnectionStatus();
}

bool ConnectionStatus::isConnected(const QString& client) const
{
    return m_Clients.contains(client, Qt::CaseInsensitive);
}
