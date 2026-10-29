/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

// TODO: fix, tests failing intermittently on mac.
#ifndef WINAPI_CARBON

#define INPUTLEAP_TEST_ENV

#include "test/mock/server/MockConfig.h"
#include "test/mock/server/MockPrimaryClient.h"
#include "test/mock/inputleap/MockScreen.h"
#include "test/mock/server/MockInputFilter.h"
#include "test/global/TestEventQueue.h"
#include "server/Server.h"
#include "server/ClientListener.h"
#include "server/ClientProxy.h"
#include "client/Client.h"
#include "inputleap/FileChunk.h"
#include "inputleap/FileClip.h"
#include "inputleap/StreamChunker.h"
#include "net/SocketMultiplexer.h"
#include "net/NetworkAddress.h"
#include "net/TCPSocketFactory.h"
#include "mt/Thread.h"
#include "base/Log.h"
#include <stdexcept>

#include <gtest/gtest.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdio.h>

namespace inputleap {

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::Invoke;

#define TEST_PORT 24803
#define TEST_HOST "localhost"

const size_t kMockDataSize = 1024 * 1024 * 10; // 10MB
const std::uint16_t kMockDataChunkIncrement = 1024; // 1KB
const char* kMockFilename = "NetworkTests.mock";
const size_t kMockFileSize = 1024 * 1024 * 10; // 10MB

void getScreenShape(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h);
void getCursorPos(std::int32_t& x, std::int32_t& y);
std::uint8_t* newMockData(size_t size);
void createFile(std::fstream& file, const char* filename, size_t size);

class NetworkTests : public ::testing::Test
{
public:
    NetworkTests() :
        m_mockData(nullptr),
        m_mockDataSize(0),
        m_mockFileSize(0)
    {
        m_mockData = newMockData(kMockDataSize);
        createFile(m_mockFile, kMockFilename, kMockFileSize);
    }

    ~NetworkTests()
    {
        remove(kMockFilename);
        delete[] m_mockData;
    }

    void sendMockData(const EventTarget* eventTarget);

    void sendToClient_mockData_handle_client_connected(const Event&, ClientListener* listener);
    void sendToClient_mockData_file_receive_completed(const Event&);

    void sendToClient_mockFile_handle_client_connected(const Event&, ClientListener* listener);
    void sendToClient_mockFile_file_receive_completed(const Event& event);

    void sendToServer_mockData_handle_client_connected(const Event&, Client* client);
    void sendToServer_mockData_file_receive_completed(const Event& event);

    void sendToServer_mockFile_handle_client_connected(const Event&, Client* client);
    void sendToServer_mockFile_file_recieve_completed(const Event& event);

public:
    TestEventQueue        m_events;
    std::uint8_t* m_mockData;
    size_t                m_mockDataSize;
    std::fstream m_mockFile;
    size_t                m_mockFileSize;
};

TEST_F(NetworkTests, sendToClient_mockData)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [this, &listener](const auto& e)
    {
        sendToClient_mockData_handle_client_connected(e, &listener);
    });

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));


    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.add_handler(EventType::FILE_RECEIVE_COMPLETED, &client,
                         [this](const auto& e)
    {
        sendToClient_mockData_file_receive_completed(e);
    });

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.remove_handler(EventType::FILE_RECEIVE_COMPLETED, &client);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToClient_mockFile)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events, ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [this, &listener](const auto& e)
    {
        sendToClient_mockFile_handle_client_connected(e, &listener);
    });

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));


    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.add_handler(EventType::FILE_RECEIVE_COMPLETED, &client,
                         [this](const auto& e)
    {
        sendToClient_mockFile_file_receive_completed(e);
    });

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.remove_handler(EventType::FILE_RECEIVE_COMPLETED, &client);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToServer_mockData)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);
    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events, ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [this, &client](const auto& e)
    {
        sendToServer_mockData_handle_client_connected(e, &client);
    });

    m_events.add_handler(EventType::FILE_RECEIVE_COMPLETED, &server,
                         [this](const auto& e)
    {
        sendToServer_mockData_file_receive_completed(e);
    });

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.remove_handler(EventType::FILE_RECEIVE_COMPLETED, &server);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToServer_mockFile)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events, ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [this, &client](const auto& e)
    {
        sendToServer_mockFile_handle_client_connected(e, &client);
    });

    m_events.add_handler(EventType::FILE_RECEIVE_COMPLETED, &server,
                         [this](const auto& e)
    {
        sendToServer_mockFile_file_recieve_completed(e);
    });

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.remove_handler(EventType::FILE_RECEIVE_COMPLETED, &server);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, filePaste_filesCopiedOnClientArePastedOnServer)
{
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);
    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    // the files the user copies on the client
    FileClip clip;
    clip.files.push_back({"report.pdf", 1234, 5678});
    clip.id = FileClip::make_id({"C:\\report.pdf"}, clip.files);
    ON_CALL(clientScreen, getClipboard(_, _))
        .WillByDefault(Invoke([&clip](ClipboardID, IClipboard* clipboard) {
            clipboard->open(0);
            clipboard->clear();
            clipboard->add(IClipboard::kFiles, clip.marshall());
            clipboard->close();
            return true;
        }));

    // once connected, the user copies the files on the client, which isn't
    // the active screen, so it sends them to the server straight away
    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [&](const auto&)
    {
        server.adoptClient(listener.getNextClient());
        m_events.add_event(EventType::CLIPBOARD_GRABBED, clientScreen.get_event_target(),
                           create_event_data<IScreen::ClipboardInfo>(
                               IScreen::ClipboardInfo{kClipboardClipboard, 0}));
    });

    // when the server has the files on its clipboard, the user pastes them on
    // the server: once with the id of an earlier copy, then with the right one
    FilePasteRequest stale{"00000000000000aa", "0000000000000000", "100.81.171.127"};
    FilePasteRequest paste{"00000000000000bb", clip.id, "100.81.171.127"};
    ON_CALL(primaryClient, setClipboard(_, _))
        .WillByDefault(Invoke([&](ClipboardID id, const IClipboard* clipboard) {
            clipboard->open(0);
            bool has_files = clipboard->has(IClipboard::kFiles);
            clipboard->close();
            if (id == kClipboardClipboard && has_files) {
                for (const auto& request : {stale, paste}) {
                    m_events.add_event(EventType::FILE_PASTE_REQUESTED,
                                       primaryClient.get_event_target(),
                                       create_event_data<FilePasteRequest>(request));
                }
            }
        }));

    // the client is asked to send the files, and reports back
    EXPECT_CALL(clientScreen, send_files(_))
        .WillOnce(Invoke([&](const FilePasteRequest& request) {
            EXPECT_EQ(paste.request, request.request);
            EXPECT_EQ(paste.file_id, request.file_id);
            EXPECT_EQ(paste.address, request.address);
            for (FilePasteState state : {FilePasteState::SENDING, FilePasteState::SENT}) {
                m_events.add_event(EventType::FILE_PASTE_STATUS, clientScreen.get_event_target(),
                                   create_event_data<FilePasteStatus>(
                                       FilePasteStatus{request.request, state, "detail"}));
            }
        }));

    // and the server's screen hears how each paste went
    std::vector<FilePasteStatus> statuses;
    EXPECT_CALL(primaryClient, file_paste_status(_))
        .WillRepeatedly(Invoke([&](const FilePasteStatus& status) {
            statuses.push_back(status);
            if (status.state == FilePasteState::SENT) {
                m_events.raiseQuitEvent();
            }
        }));

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.cleanupQuitTimeout();

    ASSERT_EQ(3u, statuses.size());
    EXPECT_EQ(stale.request, statuses[0].request);
    EXPECT_EQ(FilePasteState::FAILED, statuses[0].state);
    EXPECT_EQ("the files are no longer on the clipboard", statuses[0].detail);
    EXPECT_EQ(paste.request, statuses[1].request);
    EXPECT_EQ(FilePasteState::SENDING, statuses[1].state);
    EXPECT_EQ("detail", statuses[1].detail);
    EXPECT_EQ(paste.request, statuses[2].request);
    EXPECT_EQ(FilePasteState::SENT, statuses[2].state);
}

TEST_F(NetworkTests, filePaste_cancelOnTheServerStopsTheClientSending)
{
    NetworkAddress serverAddress(TEST_HOST, TEST_PORT);
    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    ClientListener listener(serverAddress,
                            std::make_unique<TCPSocketFactory>(&m_events, &serverSocketMultiplexer),
                            &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<MockPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    server.m_mock = true;
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    // the user copies files on the client, and pastes them on the server
    FileClip clip;
    clip.files.push_back({"big.iso", 5000, 5678});
    clip.id = FileClip::make_id({"C:\\big.iso"}, clip.files);
    ON_CALL(clientScreen, getClipboard(_, _))
        .WillByDefault(Invoke([&clip](ClipboardID, IClipboard* clipboard) {
            clipboard->open(0);
            clipboard->clear();
            clipboard->add(IClipboard::kFiles, clip.marshall());
            clipboard->close();
            return true;
        }));
    m_events.add_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener,
                         [&](const auto&)
    {
        server.adoptClient(listener.getNextClient());
        m_events.add_event(EventType::CLIPBOARD_GRABBED, clientScreen.get_event_target(),
                           create_event_data<IScreen::ClipboardInfo>(
                               IScreen::ClipboardInfo{kClipboardClipboard, 0}));
    });
    FilePasteRequest paste{"00000000000000cc", clip.id, "100.81.171.127"};
    ON_CALL(primaryClient, setClipboard(_, _))
        .WillByDefault(Invoke([&](ClipboardID id, const IClipboard* clipboard) {
            clipboard->open(0);
            bool has_files = clipboard->has(IClipboard::kFiles);
            clipboard->close();
            if (id == kClipboardClipboard && has_files) {
                m_events.add_event(EventType::FILE_PASTE_REQUESTED,
                                   primaryClient.get_event_target(),
                                   create_event_data<FilePasteRequest>(paste));
            }
        }));

    // the client starts sending, and says how far it has got
    EXPECT_CALL(clientScreen, send_files(_))
        .WillOnce(Invoke([&](const FilePasteRequest& request) {
            m_events.add_event(EventType::FILE_PASTE_STATUS, clientScreen.get_event_target(),
                               create_event_data<FilePasteStatus>(FilePasteStatus{
                                   request.request, FilePasteState::SENDING,
                                   file_paste_progress(1000, 5000)}));
        }));

    // which the server's screen shows, and the user there cancels the paste
    std::vector<FilePasteStatus> server_heard;
    EXPECT_CALL(primaryClient, file_paste_status(_))
        .WillRepeatedly(Invoke([&](const FilePasteStatus& status) {
            server_heard.push_back(status);
            if (status.state == FilePasteState::SENDING) {
                m_events.add_event(EventType::FILE_PASTE_STATUS, primaryClient.get_event_target(),
                                   create_event_data<FilePasteStatus>(FilePasteStatus{
                                       status.request, FilePasteState::FAILED,
                                       "the paste was cancelled"}));
            }
        }));

    // so the client is told to stop
    std::vector<FilePasteStatus> client_heard;
    EXPECT_CALL(clientScreen, file_paste_status(_))
        .WillRepeatedly(Invoke([&](const FilePasteStatus& status) {
            client_heard.push_back(status);
            m_events.raiseQuitEvent();
        }));

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    m_events.remove_handler(EventType::CLIENT_LISTENER_CONNECTED, &listener);
    m_events.cleanupQuitTimeout();

    ASSERT_EQ(1u, server_heard.size());
    EXPECT_EQ(file_paste_progress(1000, 5000), server_heard[0].detail);
    ASSERT_EQ(1u, client_heard.size());
    EXPECT_EQ(paste.request, client_heard[0].request);
    EXPECT_EQ(FilePasteState::FAILED, client_heard[0].state);
    EXPECT_EQ("the paste was cancelled", client_heard[0].detail);
}

void NetworkTests::sendToClient_mockData_handle_client_connected(const Event&,
                                                                 ClientListener* listener)
{
    Server* server = listener->getServer();

    ClientProxy* client = listener->getNextClient();
    if (client == nullptr) {
        throw std::runtime_error("client is null");
    }

    BaseClientProxy* bcp = client;
    server->adoptClient(bcp);
    server->setActive(bcp);

    sendMockData(server);
}

void NetworkTests::sendToClient_mockData_file_receive_completed(const Event& event)
{
    auto* client = const_cast<Client*>(static_cast<const Client*>(event.getTarget()));
    EXPECT_TRUE(client->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void NetworkTests::sendToClient_mockFile_handle_client_connected(const Event&,
                                                                 ClientListener* listener)
{
    Server* server = listener->getServer();

    ClientProxy* client = listener->getNextClient();
    if (client == nullptr) {
        throw std::runtime_error("client is null");
    }

    BaseClientProxy* bcp = client;
    server->adoptClient(bcp);
    server->setActive(bcp);

    server->sendFileToClient(kMockFilename);
}

void NetworkTests::sendToClient_mockFile_file_receive_completed(const Event& event)
{
    Client* client = const_cast<Client*>(static_cast<const Client*>(event.getTarget()));
    EXPECT_TRUE(client->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void NetworkTests::sendToServer_mockData_handle_client_connected(const Event&, Client* client)
{
    sendMockData(client);
}

void NetworkTests::sendToServer_mockData_file_receive_completed(const Event& event)
{
    Server* server = const_cast<Server*>(static_cast<const Server*>(event.getTarget()));
    EXPECT_TRUE(server->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void NetworkTests::sendToServer_mockFile_handle_client_connected(const Event&, Client* client)
{
    client->sendFileToServer(kMockFilename);
}

void NetworkTests::sendToServer_mockFile_file_recieve_completed(const Event& event)
{
    Server* server = const_cast<Server*>(static_cast<const Server*>(event.getTarget()));
    EXPECT_TRUE(server->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void NetworkTests::sendMockData(const EventTarget* event_target)
{
    // send first message (file size)
    FileChunk size_message = FileChunk::start(kMockDataSize);

    m_events.add_event(EventType::FILE_CHUNK_SENDING, event_target,
                       create_event_data<FileChunk>(size_message));

    // send chunk messages with incrementing chunk size
    size_t lastSize = 0;
    size_t sentLength = 0;
    while (true) {
        size_t dataSize = lastSize + kMockDataChunkIncrement;

        // make sure we don't read too much from the mock data.
        if (sentLength + dataSize > kMockDataSize) {
            dataSize = kMockDataSize - sentLength;
        }

        // first byte is the chunk mark, last is \0
        FileChunk chunk = FileChunk::data(m_mockData, dataSize);
        m_events.add_event(EventType::FILE_CHUNK_SENDING, event_target,
                           create_event_data<FileChunk>(chunk));

        sentLength += dataSize;
        lastSize = dataSize;

        if (sentLength == kMockDataSize) {
            break;
        }

    }

    // send last message
    FileChunk transferFinished = FileChunk::end();
    m_events.add_event(EventType::FILE_CHUNK_SENDING, event_target,
                       create_event_data<FileChunk>(transferFinished));
}

std::uint8_t* newMockData(size_t size)
{
    std::uint8_t* buffer = new std::uint8_t[size];

    std::uint8_t* data = buffer;
    const std::uint8_t head[] = "mock head... ";
    size_t headSize = sizeof(head) - 1;
    const std::uint8_t tail[] = "... mock tail";
    size_t tailSize = sizeof(tail) - 1;
    const std::uint8_t test_data[] = "test\0 data! ";
    size_t test_data_size = sizeof(test_data) - 1;

    memcpy(data, head, headSize);
    data += headSize;

    size_t times = (size - headSize - tailSize) / test_data_size;
    for (size_t i = 0; i < times; ++i) {
        memcpy(data, test_data, test_data_size);
        data += test_data_size;
    }

    size_t remainder = (size - headSize - tailSize) % test_data_size;
    if (remainder != 0) {
        memset(data, '.', remainder);
        data += remainder;
    }

    memcpy(data, tail, tailSize);
    return buffer;
}

void createFile(std::fstream& file, const char* filename, size_t size)
{
    std::uint8_t* buffer = newMockData(size);

    file.open(filename, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("file not open");
    }

    file.write(reinterpret_cast<char*>(buffer), size);
    file.close();

    delete[] buffer;
}

void getScreenShape(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h)
{
    x = 0;
    y = 0;
    w = 1;
    h = 1;
}

void getCursorPos(std::int32_t& x, std::int32_t& y)
{
    x = 0;
    y = 0;
}

} // namespace inputleap

#endif // WINAPI_CARBON
