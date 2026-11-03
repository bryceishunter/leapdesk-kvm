Release notes
=============

[comment]: <> (towncrier release notes start)

Leapdesk KVM `3.1.0` ( `2026-11-03` )
================================

========================================================================

Bug fixes
---------

- Fix the maximum clipboard size being written to the configuration file as "true",
  which the server then couldn't read. Changing the size in the window failed to save, and
  so did every change after it, including adding a computer after pairing.
- Fix configuration links written with their edge ranges rounded to whole
  percents, removing a screen from a configuration hanging, and options that were
  already set not taking a new value.
- Fix bug where UI names and '.ui' paths in language files are incorrect and not displaying correctly.
- Fix Leapdesk server inputs stuck when EIS reconnect to client after invalid EIS protocol.
- Windows: suspending and resuming the server no longer leaves it running without listening
  for clients, or unable to switch to a client because its mouse and keyboard hooks had
  gone. Clients are told again when the server closes their connection, so stopping or
  suspending the server doesn't wait out a timeout first.
- Windows: the service recognises its own server and client processes again, so a copy left
  running is shut down instead of competing with the one the service starts.
- Windows: the cursor stays hidden on a client the mouse has left, even when another
  application takes the mouse capture there or the desktop switches.

Features
--------

- Two fewer packets per input message: the client no longer answers every message with a
  no-op, and each message goes out in one write instead of two. This shows most on a
  high-latency link such as a VPN. The bytes on the wire are unchanged, so older builds
  still connect.
- Files copied in Explorer on one Windows screen can be pasted on another. They
  are sent with Taildrop when you paste, so both machines need Tailscale; the
  application you paste into waits until they have arrived, and a window shows
  how the transfer is going, with a button to cancel it. This adds protocol
  version 1.7. Older clients and servers still connect, but can't take part.
- The window edits the server's configuration file itself instead of a copy kept in its
  own settings, and keeps everything in the file it doesn't edit as it was, hotkeys it can't
  show included.
- The server's layout is set up in the window. Its Screens section draws this computer's
  monitors as Windows arranges them, marks the edges where the pointer can leave and where
  it stops, and shows each paired computer beside the stretch of edge that leads to it.
  A computer is placed by dragging it onto a monitor's edge, or by choosing the edge from a
  list, and taken off the layout by dragging it to "Not placed". The exact stretch, and where
  the pointer enters the other computer, can be set in percent, and a stretch the pointer
  can't reach is pointed out. Links between other screens are listed, and can be added,
  edited and removed, and each screen's settings are a button away. The separate Configure
  Server dialog is gone, and with it renaming screens, since a computer's name is set on it.
- New Leapdesk KVM icons and logos throughout: the programs, the installers, the
  settings window, its tray icon and About box, and the macOS and Linux icons.
  `res/make-icons.py` regenerates them all from the masters in `.assets/`.
- A Windows installer, `LeapdeskKVM-<version>.exe`, built with
  `dist/inno/build-installers.ps1`. Every computer gets every program and the Leapdesk KVM
  window, and runs them as the `Leapdesk` service, which restarts after a crash. It stops
  and removes earlier Leapdesk and Input Leap services and installs, and moves an Input
  Leap pairing certificate, trusted fingerprints and configuration over. An upgrade keeps
  the role, name and server; a new install leaves them for the window to set, or takes
  `/ROLE`, `/NAME`, `/SERVER` and `/LOGLEVEL` on the command line. The window sets the role,
  screen name and server address directly, with one administrator approval when applying,
  lists Tailscale devices to choose the server from, and shows whether the path to the
  server is direct or relayed.
- Builds against OpenSSL 3 and 4, and sets TLS 1.2 as the minimum.
- Computers pair by comparing a code. Pair a Computer on the server and Pair with
  Server on a client open a window on each that shows the same six-digit code;
  once the people at both press Pair, each trusts the other's certificate, and the
  server adds the client to its layout on the side chosen. The running server
  takes this up by itself, and a client connects again at once; pairing again
  with a computer already trusted changes nothing. A hash of the server's random nonce is sent
  before the client's nonce is seen, so someone in between can't steer the two
  codes to match. The installer lets the window take pairing on TCP port 24802,
  only over Tailscale where it can. Under the service the window points to pairing
  where it used to ask about an unknown fingerprint.
- The server reloads its configuration when the file changes, so edits apply
  without a restart; a file with an error is logged and the previous
  configuration stays in use.
- Computers can be unpaired from either side. The window lists the computers this
  one is paired with, each with an Unpair button. Unpairing takes effect within a couple
  of seconds and without a restart: the running server or client notices that the
  certificate is no longer trusted and drops the connection, and if the other computer
  is connected it is told over that connection, and forgets this one too. The server also takes the
  client off its layout. A computer that wasn't connected keeps its entry until it's
  unpaired there as well, but the two can't connect. Protocol 1.8 adds the message;
  an older peer is just disconnected.
- The window is laid out as in the new design. A status line across the top says what's
  running and whether it's connected, from what the service logs: which clients are
  connected to the server, or on a client whether it's connected, can't reach the server,
  isn't paired yet, or uses a name the server doesn't know. Server and Client are two large
  choices. The Connection section lists the computers this one is paired with, each with its
  network path, whether it's connected and an Unpair button, and the facts about this
  computer: where clients connect, its addresses, its certificate and how copy and paste works.
  On a client it checks the server address, the network path, Leapdesk on the server and
  pairing, one line each. The window can be resized and scrolls, and follows the system's
  light or dark colours and accent colour.
- Hotkeys and the server's options are edited in the window, in sections that open and
  close, and are saved to the layout file as they're changed, which the running server
  picks up within a few seconds. Each hotkey is listed with what it does in words, such as
  "Switch to desk", and is edited with its keys and its actions in one dialog. Rules the
  window can't edit are listed and kept as they are. Editing a hotkey no longer needs its
  keys pressed again before OK takes it.

Deprecations and Removals
-------------------------

- The project is now Leapdesk KVM. The programs are `leapdesk-server`,
  `leapdesk-client` and `leapdesk-daemon`, the Windows service is `Leapdesk`, and
  settings live in `%LOCALAPPDATA%\Leapdesk` (`~/.config/Leapdesk` elsewhere) with
  the certificate as `Leapdesk.pem` and the default config as `leapdesk.sgc`.
  The Windows installer replaces an Input Leap service and moves its pairing
  certificate, trusted fingerprints and configuration over; on Linux and macOS the
  first run copies the certificate and fingerprints from Input Leap's profile. The
  settings window's own settings start afresh.

Security fixes
--------------

- The Windows service no longer runs command lines sent to it on 127.0.0.1:24801,
  where any local program could connect and have a program run with the
  service's rights. It builds the server or client command itself, with the
  programs in its own folder, from settings only administrators can change:
  Role, ScreenName, ServerAddress, LogLevel and ProfileDir in
  HKLM\SOFTWARE\Leapdesk, which the installer and `leapdesk-daemon.exe /configure`
  write. A command line saved by an earlier version isn't run, since it may have
  come over IPC; running the installer again carries its screen name and server
  over. The settings window starts and stops the service through Windows instead,
  which the installer lets signed-in users do.
InputLeap `3.0.3` ( `2025-06-13` )
================================

=====================================================================

Bug fixes
---------

- Fixed cursor jumping to center of primary display when entering macOS screen by moving isOnScreen flag
- Flatpak appid has been changed to `io.github.input_leap.input-leap` to resolve to Github repository correctly.
- Fixed a segmentation fault that would occur when configuring servers on certain platforms. This solution was proposed by sithlord48 and they used it to fix the issue for deskflow.
- EI screen shape is now properly reset on update, shape x and y are not bound to 0 0 anymore.
- Fixed missing keymap configs on wayland as the XKB keymap from Ei was not properly loading. The respective keymap file was not seeked to the start on some systems.
- Fixed a problem with UTF-16 surrogate pairs that caused broken characters (especially emojis) when copying to the clipboard from a Windows machine
- Fixed input capture Disabled signal, Enable method is now properly called after Disable.
- Fixed a long standing bug with modifiers and dead keys on Windows because their state was not being reset.
- Fixed an issue with mapping keys in OSX clients introduced in #1635, where keys like the equals sign "=" or the back tick "`" wouldn't be mapped correctly.
- Remove check requiring the Alt modifier for non-extended VK_SNAPSHOT scancode in Win32.
  Check is irrelevant for modern keyboards, and breaks Print Screen functionality.

Features
--------

- Add a reload action to the tray icon menu.
  Sometimes the clipboard copy function does not work. In this case, when the input-leap is reloaded, the function works normally again.
  To make reload easier, add a reload action to the tray icon.

InputLeap `3.0.2` ( `2024-10-12` )
==================================

Bug fixes
---------

- Fixed broken screen name entry in screen settings dialog.
- Released deb packages support Wayland on Ubuntu 24.04 and 24.10.

Features
--------

- Added a Debian package for Ubuntu 24.04 and 24.10 to the set of released packages.

Deprecations and Removals
-------------------------

- Minimum MacOS version has been increased to 10.15.
- Packages for Ubuntu 20.04 are no longer distributed.

Please open a Github issue you're still using these versions and can't upgrade.  Depending on
popular demand removal of old OS support may be reverted.


InputLeap `3.0.1` ( `2024-10-10` )
==================================

Bug fixes
---------

- Added automatic user config migration from old config location on Linux.
- Fixed generation of Windows, Ubuntu, Debian and flatpak installers.


InputLeap `3.0.0` ( `2024-10-04` )
==================================

Features
--------

- Added Wayland support. Note that XWayland won't work properly and warning is printed.
- Create an `uninstall` target for Makefiles

  This allows for developers (or users who are testing) to remove Input Leap from
  their system, based on `$PREFIX`.
- Added a new clipboard sharing size limiter to prevent transferring clipboards past a default
  value in bytes.
- Allow Qt version to be selected via CMake

  CMake now uses a sane default of Qt 5, but allows for a CMake option of
  `QT_DEFAULT_MAJOR_VERSION`, for overriding of the Qt library used during build.

  For example, setting `QT_DEFAULT_MAJOR_VERSION` to `6` uses Qt 6, and setting to
  `5` uses Qt 5. Older versions are not supported.
- Added new converters for X11 to support copy pasting png/tiff/jpg/webp formats.
- Added support for building against Qt 6 in addition to Qt 5.

Bug fixes
---------

- Corrected macOS packaging to provide a better error message when a user attempts to launch
  InputLeap on an incompatible macOS version. (https://github.com/input-leap/input-leap/issues/1260).
- Removed limitation in Corner Size GUI to allow higher value than 99
  (https://github.com/input-leap/input-leap/issues/159)
- Fixed a dead lock entered when a server screen is suspended, so that screen-resume message will
  work as expected.
- Fixed spacing for max clipboard input on in the advanced server settings
  (https://github.com/input-leap/input-leap/issues/1644).
- Added a log limit to 10,000 lines to prevent RAM use from constantly increasing.
- Reduced CPU usage of log window.
- Fixed clipboard sharing toggle in GUI (https://github.com/input-leap/input-leap/issues/1789)
- Fixed generation of new certificates.
- The source distribution tarball no longer contains ``debian`` folder which previously clashed
  with data provided by the Debian packaging.
- Fixed support for debugging commpand line apps on Linux. Previously they would exit whenever debugger
  tries to interrupt them which makes debugging useless.
- Fixed out of bounds write which sometimes causes crash when switching screens.
- Fixed per-display scaling on Windows (https://github.com/input-leap/input-leap/issues/1952,
  https://github.com/input-leap/input-leap/issues/94, https://github.com/input-leap/input-leap/issues/206)
- Fixed wrong encoding for text copied between Linux and Windows
  (https://github.com/input-leap/input-leap/issues/1037,
  https://github.com/input-leap/input-leap/issues/1137).
- Fixed issue with X11 clipboard sharing images as text targets or in other targets than the one it
  should have.
- Fixed build on GCC 11.2 or newer (https://github.com/input-leap/input-leap/issues/1366).
- Fixed potential crash during app shutdown.
- Updated Japanese translation.
- Fixed "Fix preserve Focus" option on Linux servers (https://github.com/input-leap/input-leap/issues/1066).
- Fixed a potential memory leak that may cause server memory usage to grow during repeated reconnection attempts.
- Renamed references to InputLeap in translations.
- Fixed InputLeap code to produce reproducible build artifacts.
- Fixed potential crashes during server shutdown due to a stale pointer to freed memory.
- Updated Server IP shortcut to not conflict with Start/Stop button.
- Fixed bug that caused tilde (~) and grave (`) to stop being sendable.

Improved Documentation
----------------------

- Updated FAQs in project README.md with more detail on OS support and links to issues for infrequent users. (https://github.com/input-leap/input-leap/issues/1260).
- Added a note in README.md on how to obtain test builds.
- Fixed `input-leap.conf.example-advanced` to accurately include the advanced configuration described in/intended by the comment within it.
- Update LICENSE file in repository to reflect fork from Barrier organisation.
- Fixed FAQ link to Linux drag and drop issue.
- Minor grammatical updates in newsfragments README.md.

Changes
-------

- Changed server configuration dialog initial tab to "Screens".
- Changed user configuration location on Linux from ~/.local/share to ~/.config.

Deprecations and Removals
-------------------------

- The --no-xinitthreads commandline option has been deprecated and no longer has any effect.


Barrier `2.4.0` ( `2021-11-01` )
================================

Security fixes
--------------

- Barrier now supports client identity verification (fixes CVE-2021-42072, CVE-2021-42073).

  Previously a malicious client could connect to Barrier server without any authentication and
  send application-level messages. This made the attack surface of Barrier significantly larger.
  Additionally, in case the malicious client got possession of a valid screen name by brute forcing
  or other means it could modify the clipboard contents of the server.

  To support seamless upgrades from older versions of Barrier this is currently disabled by default.
  The feature can be enabled in the settings dialog. If enabled, older clients of Barrier will be
  rejected.

- Barrier now uses SHA256 fingerprints for establishing security of encrypted SSL connections.
  After upgrading client to new version the existing server fingerprint will need to be approved
  again. Client and server will show both SHA1 and SHA256 server fingerprints to allow
  interoperability with older versions of Barrier.

All of the above security issues have been reported by Matthias Gerstner who was really helpful
resolving them.

Bug fixes
---------

- Fixed build failure on mips*el and riscv64 architecture.
- Fixed reading of configuration on Windows when the paths contain non-ASCII characters
(https://github.com/input-leap/input-leap/issues/976, https://github.com/input-leap/input-leap/issues/974,
 https://github.com/input-leap/input-leap/issues/444).
- Barrier no longer uses openssl CLI tool for any operations and hooks into the openssl library directly.
- More X11 clipboard MIME types have been mapped to corresponding converters (https://github.com/input-leap/input-leap/issues/344).
- Fixed setup of multiple actions associated with a hotkey.
- Fixed setup of hotkeys with special characters such as comma and semicolon
  (https://github.com/input-leap/input-leap/issues/778).
- Fixed transfer of non-ASCII characters coming from a Windows server in certain cases
 (https://github.com/input-leap/input-leap/issues/527).
- Barrier will now regenerate server certificate if it's invalid instead of failing to launch
 (https://github.com/input-leap/input-leap/issues/802)
- Added support for additional keys on Sun Microsystems USB keyboards
 (https://github.com/input-leap/input-leap/issues/784).
- Updated Chinese translation.
- Updated Slovak translation.
- Theme icons are now preferred to icons distributed together with Barrier
 (https://github.com/input-leap/input-leap/issues/471).
- Fixed incorrect setup of Barrier service path on Windows.

Features
--------

- Added `--drop-target` option that improves drag and drop support on Windows when Barrier is
  being run as a portable app.
- The `--enable-crypto` command line option has been made the default to reduce chances of
  accidental security mishaps when configuring Barrier from command line.
  A new `--disable-crypto` command line option has been added to explicitly disable encryption.
- Added support for randomart images for easier comparison of SSL certificate fingerprints.
  The algorithm is identical to what OpenSSH uses.
- Implemented a configuration option for Server GUI auto-start.
- Made it possible to use keyboard instead of mouse to modify screen layout.
- Added support for keyboard backlight media keys
- Added support for Eisu_toggle and Muhenkan keys
- Added `--profile-dir` option that allows to select custom profile directory.

Barrier `2.3.4` ( `2021-11-01` )
================================

Security fixes
--------------

- Barrier will now correctly close connections when the app-level handshake fails (fixes CVE-2021-42075).

  Previously repeated failing connections would leak file descriptors leading to Barrier being unable
  to receive new connections from clients.

- Barrier will now enforce a maximum length of input messages (fixes CVE-2021-42076).

  Previously it was possible for a malicious client or server to send excessive length messages
  leading to denial of service by resource exhaustion.

- Fixed a bug which caused Barrier to crash when disconnecting a TCP session just after sending
  Hello message (fixes CVE-2021-42074).
  This bug allowed an unauthenticated attacker to crash Barrier with only network access.

All of the above security issues have been reported by Matthias Gerstner who was really helpful
resolving them.

Bug fixes
---------

- Fixed a bug in SSL implementation that caused invalid data occasionally being sent to clients
  under heavy load.
