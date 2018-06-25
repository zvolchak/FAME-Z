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

from twisted.internet.interfaces import IFileDescriptorReceiver

from twisted.internet.protocol import ClientFactory as TIPClientFactory
from twisted.internet.protocol import Protocol as TIPProtocol

from zope.interface import implementer

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

# The UNIX transport in the middle of this, at
# /usr/lib/python3/dist-packages/twisted/internet/unix.py line 174
# will properly glean a file descriptor if present.  There must be
# real data to go with this ancillary data.  Then, if this protocol
# is recognized as implementing IFileDescriptorReceiver, it will FIRST
# call fileDescriptorReceived before dataReceived.  So for the initial
# info exchange, version and my (new) id are put out without an fd,
# then a -1 is put out with the mailbox fd.   What triggers here is
# one fileDescriptorReceived, THEN a dataReceived of thre quad words.
# Then it pingpongs evenly between an fd and a single quadword for
# each grouping.

@implementer(IFileDescriptorReceiver)
class ProtocolIVSHMSGClient(TIPProtocol):

    IVSHMEM_PROTOCOL_VERSION = 0

    def __init__(self, cmdlineargs):
        self.args = cmdlineargs
        self.my_id = None       # Until initial info; state machine key
        self.server_id = None
        self.peer_list = OrderedDict()
        self.mailbox_fd = None
        self._latest_fd = None

    def fileDescriptorReceived(self, latest_fd):
        assert self._latest_fd is None, 'Latest fd has not been consumed'
        self._latest_fd = latest_fd
        if self.args.verbose:
            print('New FD=%d: My ID %s, mbox %s, server ID %s, peer list %s' %
                  (latest_fd, self.my_id, self.mailbox_fd,
                  self.server_id, self.peer_list.keys()))

    @property
    def latest_fd(self):
        '''This is NOT idempotent!  Could return -1 like the C version does
           but I'm partial to exceptions.'''
        assert self._latest_fd is not None, 'No fd to consume'
        tmp = self._latest_fd
        self._latest_fd = None
        return tmp

    def dataReceived(self, data):
        if self.my_id is None:      # Initial info
            # 3 longwords: protocol version w/o FD, my (new) ID w/o FD,
            # and then a -1 with the FD of the IVSHMEM file which is
            # delivered before this.
            assert self.mailbox_fd is None, 'mailbox fd already set'
            self.mailbox_fd = self.latest_fd
            assert len(data) == 24, 'Initial data needs three quadwords'
            version, self.my_id, minusone = struct.unpack('qqq', data)
            assert version == self.IVSHMEM_PROTOCOL_VERSION, \
                'Unxpected protocol version %d' % version
            assert minusone == -1, 'Did not get -1 with mailbox fd'
            if self.args.verbose:
                print('My ID = %d' % (self.my_id))
            return

        # Now into the stream of <peer id><eventfd> pairs.
        latest_fd = self.latest_fd
        assert len(data) == 8, 'Expecting a signed long long'
        this = struct.unpack('q', data)[0]
        assert this >= 0, 'Latest data is negative number'

        # Get a stream of batched integers, batch length == nVectors.
        # One batch for each existing peer, then the server (see
        # the "voodoo" comment in twisted_server.py).  In general the
        # batch lengths could be different for each peer, but in famez
        # they're all the same.  Just shove all fds in, including mine.

        if this == self.my_id:  # It's the last group, so retrieve server ID
            if self.server_id is None:
                self.server_id = list(self.peer_list.keys())[-1]

        try:
            self.peer_list[this].append(latest_fd)
        except KeyError as e:
            self.peer_list[this] = [latest_fd, ]

        # Got to do this inband because nothing more may be coming.
        if this == self.my_id and (
            len(self.peer_list[self.my_id]) ==
            len(self.peer_list[self.server_id])
        ):
            print('Last pass through, right?  Set up event id handlers')


        if self.args.verbose == 1:
            print('peer list is now %s' % str(self.peer_list.keys()))
        elif self.args.verbose > 1:
            for id, eventfds in self.peer_list.items():
                print(id, eventfds)

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
        return JFDI(self.args)

    def startedConnecting(self, connector):
        print('Started connecting')

    def clientConnectionFailed(self, connector, reason):
        print('Failed connection:', str(reason))

    def clientConnectionLost(self, connector, reason):
        print('Lost connection:', str(reason))

    def run(self):
        TIreactor.run()

