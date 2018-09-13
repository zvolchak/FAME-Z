#!/usr/bin/python3

# https://gist.github.com/berlincount/409a631d49c3be210abd

import json
import os
import sys

from pdb import set_trace

from klein import Klein

class MailBoxReSTAPI(object):

    app = Klein()   # Now it's decorators can be used against methods

    mb = None
    mm = None
    nClients = None
    nEvents = None
    server_id = None
    clients = None

    def __init__(self, already_initialized_FAMEZ_mailbox):
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

    def resample(self):

    @app.route('/')
    def home(self, request):
        print('Received "%s"' % request.uri.decode(), file=sys.stderr)
        return 'Okey dokey'

if __name__ == '__main__':

    from famez_mailbox import FAMEZ_MailBox

    fd = os.open(sys.argv[1], os.O_RDWR)
    mb = FAMEZ_MailBox(fd=fd, client_id=99, nodename='ReSTAPItest')
    MailBoxReSTAPI(mb).app.run('localhost', 1991)
