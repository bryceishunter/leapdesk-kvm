Computers can be unpaired from either side. The window lists the computers this
one is paired with, each with an Unpair button. Unpairing takes effect within a couple
of seconds and without a restart: the running server or client notices that the
certificate is no longer trusted and drops the connection, and if the other computer
is connected it is told over that connection, and forgets this one too. The server also takes the
client off its layout. A computer that wasn't connected keeps its entry until it's
unpaired there as well, but the two can't connect. Protocol 1.8 adds the message;
an older peer is just disconnected.
