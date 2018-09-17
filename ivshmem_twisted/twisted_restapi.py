#!/usr/bin/python3

# https://gist.github.com/berlincount/409a631d49c3be210abd

import attr
import json
import os
import struct
import sys

from collections import defaultdict, OrderedDict
from pdb import set_trace
from pprint import pformat, pprint

from klein import Klein                             # Uses default reactor...

from twisted.internet import reactor as TIreactor   # ...just like famez_server
from twisted.web import server as TWserver


class MailBoxReSTAPI(object):

    N = attr.make_class('Nodes', [])
    L = attr.make_class('Links', [])

    app = Klein()   # Now its decorators can be used on methods below
    isLeaf = True   # TWserver seems to need this

    mb = None
    mm = None
    nClients = None
    nEvents = None
    server_id = None
    nodes = None

    @classmethod
    def mb2dict(cls):
        thedict = OrderedDict((
            ('nClients', cls.nClients),
            ('server_id', cls.server_id),
        ))
        for id in range(1, cls.nClients + 1):
            offset = id * cls.mb.MAILBOX_SLOTSIZE
            # unpack produces a tuple.
            this = cls.nodes[id]
            this.id = id
            this.nodename = cls.mb.nodename(id)
            this.cclass = cls.mb.cclass(id)

        thedict['nodes'] = [ vars(n) for n in cls.nodes ]
        return thedict

    @app.route('/gimme')
    def gimme(self, request):
        reqhdrs = dict(request.requestHeaders.getAllRawHeaders())
        thedict = self.mb2dict()
        # Twisted "fixes" the case of headers.  And uses bytearrays.
        # print(reqhdrs, file=sys.stderr)
        if b'Apiversion' in reqhdrs:
            return json.dumps(thedict)
        return('<PRE>%s</PRE>' % pformat(dict(thedict)))

    @app.route('/')
    def home(self, request):
        # print('Received "%s"' % request.uri.decode(), file=sys.stderr)
        reqhdrs = dict(request.requestHeaders.getAllRawHeaders())
        return '<PRE>\n%s\nUse /gimme\n</PRE>' % '\n'.join(
            sorted([k.decode() for k in reqhdrs.keys()]))

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
        cls.nodes = [ cls.N() for _ in range(cls.nClients + 2) ]

        # Instead of this.app.run(), break it open and wait for
        # twister_server.py to finally invoke TIreactor.run() as all these
        # things use the default reactor.  Note that self.app was assigned
        # durng the class-level scan/eval of this source file.  See also
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
