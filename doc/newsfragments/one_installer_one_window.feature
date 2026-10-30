Windows has one installer, `LeapdeskKVM-<version>.exe`, in place of the separate
server and client ones. Every computer gets every program and the Leapdesk KVM
window. An upgrade keeps the role, name and server; a new install leaves them for
the window to set, or takes `/ROLE`, `/NAME`, `/SERVER` and `/LOGLEVEL` on the
command line. The window sets the role, screen name and server address directly,
with one administrator approval when applying, lists Tailscale devices to choose
the server from, and shows whether the path to the server is direct or relayed.
