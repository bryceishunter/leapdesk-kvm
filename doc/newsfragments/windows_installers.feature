A Windows installer, built with `dist/inno/build-installers.ps1`. It stops and
removes earlier Leapdesk and Input Leap services and installs, installs afresh,
moves an Input Leap pairing certificate and configuration over, and sets the
Leapdesk service to restart after a crash.
