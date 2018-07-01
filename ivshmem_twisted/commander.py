###########################################################################
# Serve as a gateway between one protocol (stdin) and the other(IVSHMSG).
# The protocol object must be established before (like in a factory)
# and must contain a "doCommand(self, cmdline)" method.  Then a line
# completion (ie, typing something and hitting ENTER) will invoke doCommand.
# Invocation:
#
#   class MyFactory(BaseFactory):
#       :
#       def buildProtocol(self, addr):
#           protobj = MyProtocol(....)
#           stdio.StandardIO(Commander(protobj))
#           return protobj
#
# Yeah, a mixin or implementer is probably the right way.  Manana.
# This approach hides everything in here.
# An error in here tends to not make it to stdout/err and usually
# severs the network connection established by the protocol object.

import os
import time

from twisted.internet.stdio import StandardIO

from twisted.protocols.basic import LineReceiver

class _proxyCommander(LineReceiver):

    delimiter = os.linesep.encode('ascii')      # Override for LineReceiver

    _prompt = b'cmd> '                          # Local use
    _commProto = None                           # Not an instance var

    def __init__(self, commProto):
        if self._commProto is not None:
            return
        self.__class__._commProto = commProto

    def connectionMade(self):   # First contact
        pass    # Could write first prompt now

    def lineReceived(self, line):
        if self._commProto.doCommand(line.decode()):
            # Else the reactor should be shutting down
            tmp = '%s> ' %self._commProto.nodename
            self.__class__._prompt = tmp.encode()
            self.transport.write(self._prompt)

###########################################################################


def Commander(protobj):
    return StandardIO(_proxyCommander(protobj))
