/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

#include "base/EventTarget.h"
#include "inputleap/Fwd.h"
#include "inputleap/IClient.h"

namespace inputleap {

class IClientConnection;
class IStream;

//! Generic proxy for client or primary
class BaseClientProxy : public IClient, public EventTarget {
public:
    /*!
    \c name is the name of the client.
    */
    BaseClientProxy(const std::string& name);
    ~BaseClientProxy();

    //! @name manipulators
    //@{

    //! Save cursor position
    /*!
    Save the position of the cursor when jumping from client.
    */
    void setJumpCursorPos(std::int32_t x, std::int32_t y);

    //! Send copied files
    /*!
    Asks this screen to send the files on its clipboard for a paste on
    another screen.  It reports back with FilePasteStatus, which the
    server passes on to the pasting screen.
    */
    virtual void send_files(const FilePasteRequest& request) { (void) request; }

    //! Notify of file paste progress
    /*!
    Tells this screen how a paste it asked for is going.
    */
    virtual void file_paste_status(const FilePasteStatus& status) { (void) status; }

    //@}
    //! @name accessors
    //@{

    //! Get cursor position
    /*!
    Get the position of the cursor when last jumping from client.
    */
    void getJumpCursorPos(std::int32_t& x, std::int32_t& y) const;

    //! Get cursor position
    /*!
    Return if this proxy is for client or primary.
    */
    virtual bool isPrimary() const { return false; }

    //! Test for file paste support
    /*!
    Return true if this screen can send and paste files copied on other
    screens, i.e. it speaks protocol 1.7 or is the primary.
    */
    virtual bool supports_file_paste() const { return false; }

    //! Test for unpairing support
    /*!
    Return true if this screen forgets the server when told it was unpaired,
    i.e. it speaks protocol 1.8.
    */
    virtual bool supports_unpair() const { return false; }

    //@}

    // IClient overrides
    virtual void sendDragInfo(std::uint32_t fileCount, const char* info, size_t size) = 0;
    virtual void file_chunk_sending(const FileChunk& chunk) = 0;
    std::string getName() const override;
    virtual IClientConnection& get_conn() const = 0;

private:
    std::string m_name;
    std::int32_t m_x, m_y;
};

} // namespace inputleap
