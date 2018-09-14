#!/usr/bin/python3

# https://gist.github.com/berlincount/409a631d49c3be210abd

import json
import os
import sys

from pdb import set_trace
from pprint import pformat, pprint

from klein import Klein     # Uses default reactor

from twisted.web import server as TWserver

class MailBoxReSTAPI(object):

    app = Klein()   # Now it's decorators can be used against methods

    isLeaf = True   # TWserver seems to need this

    mb = None
    mm = None
    nClients = None
    nEvents = None
    server_id = None
    clients = None

    @app.route('/')
    def home(self, request):
        # print('Received "%s"' % request.uri.decode(), file=sys.stderr)
        return str(pformat(vars(request)))
        if not request.requestHeaders['apiversion']:
            return "HTML sure"
        return str(pformat(vars(request)))

    # Must come after all Klein dependencies and @decorators
    def __init__(self, already_initialized_FAMEZ_mailbox, port=1991):
        cls = self.__class__
        if cls.mb is not None:
            return
        cls.mb = already_initialized_FAMEZ_mailbox
        cls.mm = cls.mb.mm
        cls.nClients = cls.mb.nClients
        cls.nEvents = cls.mb.nEvents
        cls.server_id = cls.mb.server_id
        # Clients/ports are enumerated 1-nClients inclusive
        cls.clients = list((None, ) * (cls.nClients + 1))   # skip [0]

        # def proxystart(apiobj, port=1991):
        # Instead of this.app.run(), break it open and wait for reactor.run()
        # /usr/lib/python3/dist-packages/klein/app.py::run()
        # s = TWserver.Site(apiobj.app.resource())
        s = TWserver.Site(self.app.resource())
        TIreactor.listenTCP(port, s)

if __name__ == '__main__':

    from famez_mailbox import FAMEZ_MailBox
    from twisted.internet import reactor as TIreactor

    fd = os.open(sys.argv[1], os.O_RDWR)
    mb = FAMEZ_MailBox(fd=fd, client_id=99, nodename='ReSTAPItest')
    tmp = MailBoxReSTAPI(mb)
    # proxystart(tmp)

    # This is done elsewhere in primary app
    TIreactor.run()
