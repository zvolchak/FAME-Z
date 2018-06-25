#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import grp
import mmap
import os
import struct
import sys

from collections import OrderedDict

from twisted.python import log as TPlog
from twisted.python.logfile import DailyLogFile

from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXClientEndpoint

from twisted.internet.protocol import ClientFactory as TIPClientFactory
from twisted.internet.protocol import Protocol as TIPProtocol


try:
    from ivshmem_sendrecv import ivshmem_send_one_msg, ivshmem_recv_one_msg
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
except ImportError as e:
    from .ivshmem_sendrecv import ivshmem_send_one_msg, ivshmem_recv_one_msg
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader

###########################################################################
# See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
# qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
# qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
# qemu/contrib/ivshmem-client.c::ivshmem_client_connect()


class ProtocolIVSHMSGClient(TIPProtocol):

    IVSHMEM_PROTOCOL_VERSION = 0


    def __init__(self, cmdlineargs):
        self.args = cmdlineargs
        self.my_id = None       # Until initial info; state machine key
        self.server_id = None
        self.peer_list = OrderedDict()
        self.nVectors = None

    # /usr/lib/python3/dist-packages/twisted/internet/unix.py line 174
    def fileDescriptorReceived(self, *data):
        print('FD received:', data)

    def dataReceived(self, data):
        '''sendmsg sends data here, not sure where eventfds are...'''
        # print('dataReceived', str(data))
        if self.my_id is None:
            # 3 longwords: protocol version w/o FD, my (new) ID w/o FD,
            # and then a -1 with the FD of the IVSHMEM file.  But by
            # the time I reach here, where is the FD?  Ancillary data
            # does show up in the library at
            # /usr/lib/python3/dist-packages/twisted/internet/unix.py line 174
            # but I need to get my fileDescriptorReceived in the path...
            # or the FileDescriptorReady event taken over...or something.
            # Raise an AssertionError here to get a stack trace and see the
            # unix.py thing I need to commandeer.
            try:
                version, self.my_id, junk = struct.unpack('QQQ', data)
            except Exception as e:
                raise SystemExit('Error while reading version: %s' % str(e))
            assert version == self.IVSHMEM_PROTOCOL_VERSION, \
                'Wrong protocol version'
            print('My ID = %d' % (self.my_id))
            self.nVectors = None
            return

        # Get a stream of batched integers, batch length == nVectors.
        # One batch for each existing peer, then the server (see
        # the "voodoo" comment in twisted_server.py).  In general the
        # batch lengths could be different for each peer, but in famez
        # they're all the same.  FIXME: somewhere a recvmsg should be
        # plucking the event fds.
        this = struct.unpack('Q', data)[0]
        if this != self.my_id:
            if this not in self.peer_list:
                self.peer_list[this] = []
                print('peer list is now %s' % str(self.peer_list.keys()))
            return

        # My peer id
        if self.nVectors is None:  # first time
            self.nVectors = 1
            self.server_id = list(self.peer_list.keys())[-1]
            print('Server ID =', self.server_id)
            print('peer list is now %s' % str(self.peer_list.keys()))
        else:
            self.nVectors += 1
        print('nVectors is now %d' % self.nVectors)

    def connectionMade(self):
        print('Connection made on fd', self.transport.fileno())

    def connectionLost(self, reason):
        '''Tell the other peers that I have died.'''
        if reason.check(TIError.ConnectionDone) is None:    # Dirty
            txt = 'Dirty'
        else:
            txt = 'Clean'
        print('%s disconnect' % (txt, ))

    @staticmethod
    def ERcallback(vectorobj):
        # Strings are known to be null padded.
        selph = vectorobj.cbdata
        index = vectorobj.num * 512     # start of nodename
        selph.nodename, msglen = struct.unpack('32sQ',
            selph.mailbox_mm[index:index + 40])
        selph.nodename = selph.nodename.split(b'\0', 1)[0].decode()
        index += 128
        fmt = '%ds' % msglen
        selph.msg = struct.unpack(fmt, selph.mailbox_mm[index:index + msglen])
        selph.msg = selph.msg[0].split(b'\0', 1)[0].decode()
        selph.logmsg('"%s" (%d) sends "%s"' %
            (selph.nodename, vectorobj.num, selph.msg))

    def startedConnecting(self, connector):
        print('Started connecting')

###########################################################################
# Normally the Endpoint and listen() call is done explicitly,
# interwoven with passing this constructor.  This approach hides
# all the twisted things in this module.


class FactoryIVSHMSGClient(TIPClientFactory):

    _required_arg_defaults = {
        'socketpath':   '/tmp/ivshmem_socket',
        'verbose':      0,
    }

    def __init__(self, args=None):
        '''Args must be an object with the following attributes:
           socketpath, verbose
           Suitable defaults will be supplied.'''

        # Pass command line args to ProtocolIVSHMSG, then open logging.
        if args is None:
            args = argparse.Namespace()
        for arg, default in self._required_arg_defaults.items():
            setattr(args, arg, getattr(args, arg, default))

        self.args = args

        # checkPID looks for <socketpath>.lock which the server sets up
        # as a symlink to file named <PID>
        E = UNIXClientEndpoint(
            TIreactor,
            args.socketpath,
            timeout=1,
            checkPID=False)
        E.connect(self)
        print('Connecting to %s' % args.socketpath)

    def buildProtocol(self, useless_addr):
        print('buildProtocol')
        return ProtocolIVSHMSGClient(self.args)

    def startedConnecting(self, connector):
        print('Started connecting')

    def clientConnectionFailed(self, connector, reason):
        print('Failed connection:', str(reason))

    def clientConnectionLost(self, connector, reason):
        print('Lost connection:', str(reason))

    def run(self):
        TIreactor.run()

