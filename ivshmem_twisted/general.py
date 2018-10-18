###########################################################################
# Cleaner than class variables, allows code reuse, and also leverages
# support "downstream" for interrupt handling.  A collection file for
# things under active development before they find better homes.

###########################################################################

import sys

from collections import OrderedDict


class ServerInvariant(object):

    def __init__(self, args=None):
        self.args = args
        if args is None:        # Called from client, filled in from server
            self.nClients = 0
            self.nEvents = 0
            self.server_id = 0
            self.logmsg = print
            self.logerr = print
            self.stdtrace = sys.stdout
            return

        self.logmsg = args.logmsg           # Often-used
        self.logerr = args.logerr
        self.stdtrace = sys.stderr
        self.nClients = args.nClients
        self.nEvents = args.nEvents
        self.server_id = args.server_id
        self.clients = OrderedDict()        # Order probably not necessary
        self.recycled = {}
        if args.smart:
            self.default_SID = 27
            self.server_SID0 = self.default_SID
            self.server_CID0 = args.server_id * 100
            self.isPFM = True
        else:
            self.default_SID = 0
            self.server_SID0 = 0
            self.server_CID0 = 0
            self.isPFM = False

    def trace(self, tracemsg):
        print(tracemsg, file=self.stdtrace)
