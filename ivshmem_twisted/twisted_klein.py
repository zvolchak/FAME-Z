#!/usr/bin/python3

# https://gist.github.com/berlincount/409a631d49c3be210abd

import json
import os
import sys

from collections import defaultdict
from pdb import set_trace
from pprint import pformat, pprint

from klein import Klein     # Uses default reactor

from twisted.internet import reactor as TIreactor
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

    @app.route('/gimme')
    def gimme(self, request):
        return 'Not yet\n'

    @app.route('/')
    def home(self, request):
        # print('Received "%s"' % request.uri.decode(), file=sys.stderr)
        reqhdrs = dict(request.requestHeaders.getAllRawHeaders())
        return reqhdrs.get('asdf', 'eat me\n')
        return str(request.requestHeaders.getRawHeaders('asdf'))
        if not request.requestHeaders.hasHeader('apiversion'):
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

        # Instead of this.app.run(), break it open and wait for reactor.run()
        # /usr/lib/python3/dist-packages/klein/app.py::run()
        s = TWserver.Site(self.app.resource())
        TIreactor.listenTCP(port, s)

if __name__ == '__main__':

    from famez_mailbox import FAMEZ_MailBox

    fd = os.open(sys.argv[1], os.O_RDWR)
    mb = FAMEZ_MailBox(fd=fd, client_id=99, nodename='ReSTAPItest')
    tmp = MailBoxReSTAPI(mb)

    # This is done elsewhere in real Twisted famez_server app
    TIreactor.run()
