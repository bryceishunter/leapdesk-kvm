/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#include "IpcClient.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <iostream>
#include <QTimer>
#include "IpcReader.h"
#include "Ipc.h"
#include <QDataStream>

IpcClient::IpcClient() :
m_ReaderStarted(false),
m_Enabled(false)
{
    m_Socket = new QTcpSocket(this);
    connect(m_Socket, &QTcpSocket::connected, this, &IpcClient::connected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_Socket, &QTcpSocket::errorOccurred, this, &IpcClient::error);
#else
    connect(m_Socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(error(QAbstractSocket::SocketError)));
#endif

    m_Reader = new IpcReader(m_Socket);
    connect(m_Reader, &IpcReader::readLogLine, this, &IpcClient::handleReadLogLine);
}

IpcClient::~IpcClient()
{
}

void IpcClient::connected()
{
    sendHello();
    Q_EMIT infoMessage("connection established");
}

void IpcClient::connectToHost()
{
    m_Enabled = true;

    Q_EMIT infoMessage("connecting to service...");
    m_Socket->connectToHost(QHostAddress(QHostAddress::LocalHost), IPC_PORT);

    if (!m_ReaderStarted) {
        m_Reader->start();
        m_ReaderStarted = true;
    }
}

void IpcClient::disconnectFromHost()
{
    Q_EMIT infoMessage("service disconnect");
    m_Reader->stop();
    m_Socket->close();
}

void IpcClient::error(QAbstractSocket::SocketError error)
{
    QString text;
    switch (error) {
        case 0: text = "connection refused"; break;
        case 1: text = "remote host closed"; break;
        default: text = QString("code=%1").arg(error); break;
    }

    Q_EMIT errorMessage(QString("ipc connection error, %1").arg(text));

    QTimer::singleShot(1000, this, SLOT(retryConnect()));
}

void IpcClient::retryConnect()
{
    if (m_Enabled) {
        connectToHost();
    }
}

void IpcClient::sendHello()
{
    QDataStream stream(m_Socket);
    stream.writeRawData(kIpcMsgHello, 4);

    char typeBuf[1];
    typeBuf[0] = kIpcClientGui;
    stream.writeRawData(typeBuf, 1);
}

void IpcClient::handleReadLogLine(const QString& text)
{
    Q_EMIT readLogLine(text);
}
