Releasing
=========

For maintainers of Leapdesk KVM.

1. Collect the release notes, then commit them:

       pip install towncrier
       towncrier build --version X.Y.Z --date `date -u +%F`

   `towncrier` skips fragment files whose names it doesn't recognise, so check
   `doc/newsfragments` for leftovers.

2. Update the version in `cmake/Version.cmake`, `dist/debian/changelog`, the
   `<releases>` in `res/io.github.bryceishunter.leapdesk-kvm.appdata.xml`, and
   the headers of `doc/leapdesk-server.1` and `doc/leapdesk-client.1`.

3. Build the Windows installer from a clean checkout of that commit:

       dist\inno\build-installers.ps1 -BuildDir build

   It runs the installer's self-test, and names the file
   `LeapdeskKVM-X.Y.Z-<rev>.exe`. Upload it as `LeapdeskKVM-X.Y.Z.exe`, with
   its SHA-256 (`Get-FileHash`) in a `SHA256SUMS.txt` beside it.

4. Tag and push:

       git tag -s vX.Y.Z -m vX.Y.Z
       git push origin master vX.Y.Z

5. Draft the release at
   https://github.com/bryceishunter/leapdesk-kvm/releases, using the tag as the
   title. The description says what works, the known issues and how to install,
   followed by the collected notes. Attach the installer and `SHA256SUMS.txt`.
