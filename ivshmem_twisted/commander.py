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
#           Commander(protobj)
#           return protobj
#
# Yeah, a mixin or implementer is probably the right way.  Manana.
# This approach hides everything in here.
# An error in here tends to not make it to stdout/err and usually
# severs the network connection established by the protocol object.

import os

from twisted.internet import reactor as TIreactor

from twisted.internet.stdio import StandardIO   # internally hooks into "reactor"

from twisted.protocols.basic import LineReceiver

class _proxyCommander(LineReceiver):

    delimiter = os.linesep.encode('ascii')      # Override for LineReceiver

    _once = None

    def __init__(self, commProto):
        if self._once is not None:
            return _once
        self._once = self
        self.commProto = commProto
        self.jfdi = getattr(commProto, 'doCommand', self.doCommand)
        print('Ready')

    def connectionMade(self):   # First contact
        pass    # Could write first prompt now

    def connectionLost(self, reason):
        if self.jfdi == self.doCommand:     # doing it to myself
            TIreactor.stop()

    def doCommand(self, cmdline):
        '''The default command line processor:  help and quit.'''
        if not cmdline.strip():
            return True     # "Keep going"
        elems = cmdline.split()
        cmd = elems.pop(0)
        if cmd in ('h', 'help', 'l', 'list') or '?' in cmd:
            print('h[elp]\n\tThis message')
            print('q[uit]\n\tJust do it')
        elif cmd.startswith('q'):
            self.transport.loseConnection()
            return False
        return True

    def lineReceived(self, line):
        if self.jfdi(line.decode()):
            # Else the reactor should be shutting down.  If not, get the
            # nodename each time cuz client's is not known at __init__ time.
            try:
                nodename = self.commProto.nodename
            except AttributeError as e:
                nodename = 'cmd'
            tmp = '%s> ' % nodename
            self.prompt = tmp.encode()
            self.transport.write(self.prompt)

###########################################################################


def Commander(protobj):
    return StandardIO(_proxyCommander(protobj))
