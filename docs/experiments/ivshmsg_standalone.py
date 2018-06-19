#!/usr/bin/python3

import logging
import os
import struct
import sys

from pdb import set_trace

from twisted.python import log as TWIPlog
from twisted.internet import protocol, reactor, endpoints

class IVSHMEMProtocol(protocol.Protocol):

    IVSHMEM_PROTOCOL_VERSION = 0

    next_ivshmem_id = 1     # Docs says zero okay, not my experience

    def connectionLost(self, byebye):
        TWIPlog.msg('connectionLost', logLevel=logging.CRITICAL)

    def connectionMade(self):
        TWIPlog.msg('connectionMade to %s' % self.transport.getPeer())

        # See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol
        self.transport.write(struct.pack('Q', self.IVSHMEM_PROTOCOL_VERSION))
        self.transport.write(struct.pack('Q', self.next_ivshmem_id))
        self.next_ivshmem_id += 1
        self.transport.write(struct.pack('q', -1))   # Kick FD
        self.transport.sendFileDescriptor(42)   # No IVSHMEM fd here

    def dataReceived(self, data):
        TWIPlog.msg('dataReceived', logLevel=logging.CRITICAL)
        self.transport.write(data)

class IVSHMEMFactory(protocol.Factory):
    def buildProtocol(self, addr):
        TWIPlog.msg('buildProtocol', logLevel=logging.CRITICAL)
        return IVSHMEMProtocol()

TWIPlog.startLogging(sys.stdout)

E = endpoints.serverFromString(reactor,
    'unix:address=/tmp/ivshmem_socket:mode=666:lockfile=1')

E.listen(IVSHMEMFactory())
reactor.run()
