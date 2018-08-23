###########################################################################
# Cleaner than class variables, allows code reuse, and also leverages
# support "downstream" for interrupt handling.  A collection file for
# things under active development before they find better homes.

###########################################################################

import sys

from collections import OrderedDict


class ServerInvariant(object):

    def __init__(self, args=None):
        if args is None:        # Called from client, filled in gradually
            self.nClients = 0   # Total peers including me, excluding server
            self.nEvents = 0
            self.server_id = 0
            self.logmsg = print
            self.logerr = print
            self.stdtrace = sys.stdout
            return

        self.args = args
        self.logmsg = args.logmsg           # Often-used
        self.logerr = args.logerr
        self.stdtrace = sys.stderr
        self.nClients = args.nClients
        self.server_id = args.nClients + 1  # This is me!
        self.nEvents = args.nClients + 2
        self.clients = OrderedDict()        # Order probably not necessary
        self.recycled = {}
        if args.smart:
            self.default_SID = 27
            self.server_SID0 = self.default_SID
            self.server_CID0 = self.server_id * 100
            self.isPFM = True
        else:
            self.default_SID = 0
            self.server_SID0 = 0
            self.server_CID0 = 0
            self.isPFM = False

    def trace(self, tracemsg):
        print(tracemsg, file=self.stdtrace)
