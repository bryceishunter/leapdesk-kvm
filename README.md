# Leapdesk KVM

Share one keyboard and mouse across several computers. Move the pointer off the
edge of one screen and it appears on the next machine; the clipboard follows.
It works over any IP network — a LAN, Tailscale, WireGuard, whatever you have.

Leapdesk KVM continues [Input Leap](https://github.com/input-leap/input-leap),
which is no longer maintained. Input Leap forked Barrier, which forked Synergy
1.x, and the goal has not changed since: one keyboard and mouse, several
machines, nothing else.

Not affiliated with Synergy, Deskflow, or Tailscale.

## Status

Small and active. Leapdesk KVM is developed and tested on 64-bit Windows, and
the Windows installer is the one supported package. The macOS,
Linux and BSD code is inherited from Input Leap and isn't tested here; build it
from source, and expect rough edges. Reports and patches from those platforms
are welcome.

What changed since Input Leap 3.0.3 is in the
[release notes](doc/release_notes/index.md), and each
[release](https://github.com/bryceishunter/leapdesk-kvm/releases) lists its
known issues.

## Install on Windows

Each computer needs 64-bit Windows 11, or Windows 10 version 1809 or later, and
an administrator to approve the install. [Tailscale](https://tailscale.com) on every computer is recommended:
the installer then opens Leapdesk's ports on the Tailscale network only, the
window can pick the server from your devices, and copied files can be pasted
across.

1. Download `LeapdeskKVM-<version>.exe` from the
   [latest release](https://github.com/bryceishunter/leapdesk-kvm/releases/latest)
   onto every computer, and run it on each. The installer isn't code-signed yet,
   so Windows SmartScreen may say it protected your PC: choose **More info**,
   then **Run anyway**.
2. When it finishes, the Leapdesk KVM window opens. On the computer whose
   keyboard and mouse you want to share, choose **Server**. On each of the
   others, choose **Client** and enter the server's address, or use
   **Choose from Tailscale...**. Press **Apply** on each, and approve the prompt.
3. Pair each client with the server: press **Pair a Computer...** on the server
   and **Pair with Server...** on the client. Both show a six-digit code. Check
   that the codes match, choose under **Place it:** which side of the server the
   client is on, and press **Pair** on both.
4. The client connects within a few seconds. On the server, the **Screens**
   section shows your monitors with the client beside one of their edges; drag
   it to the edge where the pointer should cross over.

Leapdesk runs as the `Leapdesk` service, so it also works on the sign-in screen
and in administrator windows. The window doesn't need to stay open; it lives in
the notification area. Pairing certificates, the server's layout and the logs
are kept in `%LOCALAPPDATA%\Leapdesk` of the user who installed it.

Installing over Input Leap removes its service and programs, and carries its
pairing certificate, trusted computers and server layout over. Installing over
an earlier Leapdesk keeps the role, name and server address. Uninstalling, from
**Settings > Apps**, removes the programs, the service and the firewall rules,
and keeps `%LOCALAPPDATA%\Leapdesk`.

For a silent install, run the installer with `/VERYSILENT` and give the role on
the command line: `/ROLE=Server` or `/ROLE=Client /SERVER=<host or host:port>`,
and optionally `/NAME=<screen name>` and `/LOGLEVEL=<level>`.

## Copy and paste files

On Windows, files copied in Explorer on one screen can be pasted on another.
Copy them as usual, move to the other screen and paste. The screen that copied
the files sends them with [Taildrop](https://tailscale.com/kb/1106/taildrop)
when they're asked for, once per paste, and the application you paste into
waits until they have arrived. A paste that takes more than a moment opens a
Leapdesk window showing how much has arrived, how fast and how long is left,
with a Cancel button that stops the transfer on both machines. Explorer also
asks for the files while you work in it, so the transfer can start a moment
before you paste. Copying anything else first replaces them, like any other
clipboard content.

Both machines need Tailscale on the same tailnet, with Taildrop allowed, and
this version of Leapdesk; an older one still connects but can't take part.
Folders can't be copied this way yet. The files pass through your Downloads
folder, where Taildrop saves them, and are kept in
`%LOCALAPPDATA%\Leapdesk\Pasted` until the clipboard changes.

## Build on Windows

Needs Visual Studio 2022 or 2026 with the C++ tools, CMake 3.21+, OpenSSL and,
for the window, Qt 6.2 or later for MSVC.

```
winget install --id ShiningLight.OpenSSL.Dev
git clone --recurse-submodules https://github.com/bryceishunter/leapdesk-kvm
cd leapdesk-kvm
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DINPUTLEAP_BUILD_GUI=ON -DINPUTLEAP_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64 -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build --config Release
```

Use `-G "Visual Studio 17 2022"` on VS 2022, and point `CMAKE_PREFIX_PATH` at
your Qt. Binaries land in `build/bin/Release`; with
`-DINPUTLEAP_BUILD_GUI=OFF` only the command-line programs are built, and Qt
isn't needed. The Bonjour SDK is optional: without it, the window is built
without automatic discovery of servers.

To build the installer as well, install Inno Setup 6
(`winget install JRSoftware.InnoSetup --scope user`) and run
`dist\inno\build-installers.ps1 -BuildDir build`. It builds, runs the
installer's self-test and writes `build\installers\LeapdeskKVM-<version>-<rev>.exe`.

On Linux, macOS and BSD, see the build steps in the Input Leap wiki; they still
apply.

## Command line

The installer and the window are the supported way to run Leapdesk on Windows.
The programs can also be run by hand, as on other platforms. The server is the
machine whose keyboard and mouse you are sharing. Write a config naming both
screens and how they are arranged:

```
section: screens
    desktop:
    laptop:
end
section: links
    desktop:
        right = laptop
    laptop:
        left = desktop
end
```

Screen names must match each machine's `--name` (its hostname by default).

```
# on the server
leapdesk-server -f --name desktop -c leapdesk.conf

# on the other machine
leapdesk-client -f --name laptop 192.0.2.10:24800
```

The server listens on TCP port 24800, and reloads its config when the file
changes. The address must be the last argument on the client. `--help` lists the
rest.

## Encryption

TLS is on by default, and each side checks the other's certificate fingerprint.
The Leapdesk KVM window creates each computer's certificate and pairs computers
by comparing a code, as above. Without the window, generate a certificate per
machine as `<profile>/SSL/Leapdesk.pem` (on Windows, `%LOCALAPPDATA%\Leapdesk`)
and put each machine's SHA-256 fingerprint in the other's `SSL/Fingerprints/`
file as `v2:sha256:<hex>`.

On a network that is already private and authenticated, `--disable-crypto` on
both sides skips all of that. Note that this also removes authentication:
anything that can reach port 24800 can then connect and receive your keystrokes.

## Contributing

Issues and pull requests are welcome. Keep changes focused, and match the style
of the code around them. User-visible changes get a file in `doc/newsfragments`
(see the README there).

## License

GPL-2.0-only, inherited from Input Leap, Barrier and Synergy 1.x. See `LICENSE`.
