#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import grp
import mmap
import struct
import sys

from collections import OrderedDict

from twisted.internet import stdio
from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXClientEndpoint

from twisted.internet.interfaces import IFileDescriptorReceiver

from twisted.internet.protocol import ClientFactory as TIPClientFactory
from twisted.internet.protocol import Protocol as TIPProtocol

from zope.interface import implementer

try:
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader, IVSHMEM_Event_Notifier
    from famez_mailbox import FAMEZ_MailBox
    from commander import Commander
except ImportError as e:
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader, IVSHMEM_Event_Notifier
    from .famez_mailbox import FAMEZ_MailBox
    from .commander import Commander

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

@implementer(IFileDescriptorReceiver)   # Energizes fileDescriptorReceived
class ProtocolIVSHMSGClient(TIPProtocol):

    CLIENT_IVSHMEM_PROTOCOL_VERSION = 0

    def __init__(self, cmdlineargs):
        self.args = cmdlineargs
        self.my_id = None       # Until initial info; state machine key
        self._nodename = None   # Generate it for myself, retrieve for peers
        self.server_id = None
        self.fd_list = OrderedDict()    # Sent to me
        self.peer_list = OrderedDict()  # Calculated at end
        self.mailbox = None
        self._latest_fd = None
        self.id2nodename = None
        self.nSlots = 0
        # The state machine major decisions about the semantics of blocks of
        # data have one predicate.   While technically unecessary, firstpass
        # guards against server errors.
        self.firstpass = True

    @property   # For Commander prompt
    def nodename(self):
        return self._nodename

    def get_nodenames(self):
        self.id2nodename = OrderedDict()
        for id in sorted(self.fd_list):   # integer keys: their ID
            nodename, _ = self.mailbox.pickup_from_slot(id)
            self.id2nodename[id] = nodename

    def parse_target(self, instr):
        '''Return a list even for one item for consistency with keywords
           ALL and OTHERS.'''
        self.get_nodenames()
        indices = tuple()       # Default return is nothing
        try:
            indices = (int(instr), )
        except TypeError as e:
            return indices
        except ValueError as e:
            if instr.lower() == 'server':
                return (self.server_id,)
            elif instr.lower() == 'all':
                return sorted(self.id2nodename.keys())
            elif instr.lower() == 'others':
                tmp = list(self.id2nodename.keys())
                del tmp[self.my_id]
                return sorted(tmp)

            for id, nodename in self.id2nodename.items():
                if nodename == instr:
                    indices = (id, )
                    break
        return indices

    def place_and_go(self, dest, msg, src=None):
        dest_indices = self.parse_target(dest)
        if src is None:
            src_indices = (self.my_id,)
        else:
            src_indices = self.parse_target(src)
        if self.args.verbose > 1:
            print('P&G dest %s=%s src %s=%s' %
                      (dest, dest_indices, src, src_indices))
        assert src_indices and dest_indices, 'P&G: bad indices'
        for S in src_indices:
            self.mailbox.place_in_slot(S, msg)
            for D in dest_indices:
                if self.args.verbose > 1:
                    print('P&G(%s, "%s", %s)' % (D, msg, S))
                try:
                    self.peer_list[D][S].incr()
                except Exception as e:
                    print('place_and_go(%s, "%s", %s) failed: %s' %
                        (D, msg, S, str(e)))
                    pass    # Soldier on!

    def fileDescriptorReceived(self, latest_fd):
        assert self._latest_fd is None, 'Latest fd has not been consumed'
        self._latest_fd = latest_fd     # See the next property

    @property
    def latest_fd(self):
        '''This is NOT idempotent!'''
        tmp = self._latest_fd
        self._latest_fd = None
        return tmp

    def dataReceived(self, data):
        if self.my_id is None and self.firstpass:      # Initial info
            # 3 longwords: protocol version w/o FD, my (new) ID w/o FD,
            # and then a -1 with the FD of the IVSHMEM file which is
            # delivered before this.
            assert len(data) == 24, 'Initial data needs three quadwords'
            assert self.mailbox is None, 'mailbox already set'

            # Enough idiot checks.
            mailbox_fd = self.latest_fd
            version, self.my_id, minusone = struct.unpack('qqq', data)
            assert version == self.CLIENT_IVSHMEM_PROTOCOL_VERSION, \
                'Unxpected protocol version %d' % version
            assert minusone == -1, \
                'Expected -1 with mailbox fd, got %d' % minuseone
            self._nodename = 'z%02d' % self.my_id
            print('This ID = %2d (%s)' % (self.my_id, self.nodename))

            # Initialize my mailbox slot
            self.mailbox = FAMEZ_MailBox(
                mailbox_fd, peer_id=self.my_id, nodename=self.nodename)
            self.nSlots = self.mailbox.nSlots
            return

        # Now into the stream of <peer id><eventfd> pairs.  Unless it's
        # a single <peer id> which is a disconnect notification.
        latest_fd = self.latest_fd
        assert len(data) == 8, 'Expecting a signed long long'
        this = struct.unpack('q', data)[0]
        if self.args.verbose > 1:
            print('Just got index %s, fd %s' % (this, latest_fd))
        assert this >= 0, 'Latest data is negative number'

        if latest_fd is None:   # "this" is a disconnect notification
            print('%s (%d) has left the building' %
                (self.id2nodename[this], this))
            for collection in (self.peer_list, self.id2nodename, self.fd_list):
                try:
                    del collection[this]
                except Exception as e:
                    pass
            return

        # Get a stream of batched integers, batch length == nSlots.
        # One batch for each existing peer, then the server (see
        # the "voodoo" comment in twisted_server.py).  In general the
        # batch lengths could be different for each peer, but in famez
        # they're all the same.  Just shove all fds in, including mine.

        # Am I starting the last batch (eventfds for me that need notifiers?)
        if this == self.my_id and self.server_id is None:
            self.server_id = self.prevthis
        self.prevthis = this     # For corner case where I am first contact

        # Just save the eventfd now, generate objects later.
        try:
            tmp = len(self.fd_list[this])
            assert tmp <= self.nSlots, 'fd list is too long'
            if tmp == self.nSlots:      # Beginning of client reconnect
                assert this != self.my_id, 'Updating MY eventfds????'
                raise KeyError('Forced update')
            self.fd_list[this].append(latest_fd)   # order matters
        except KeyError as e:
            self.fd_list[this] = [latest_fd, ]

        if self.args.verbose > 1:
            print('fd list is now %s' % str(self.fd_list.keys()))
            for id, eventfds in self.fd_list.items():
                print(id, eventfds)

        # Do the final housekeeping after the final batch.  ASS-U-MES all
        # vector lists are the same length.  My vectors come last during
        # first pass.  During a new client join it's only their info.
        if ((self.firstpass and this != self.my_id) or
            (len(self.fd_list[this]) < self.nSlots)):
            return

        # First convert all fds to event notifier objects for both signalling
        # to other peers and waiting on signals to me.
        if self.args.verbose:
            print('--------- Finish housekeeping')
        for id in self.fd_list:     # For triggering message pickup
            if id not in self.peer_list:
                self.peer_list[id] = ivshmem_event_notifier_list(
                    self.fd_list[id])

        # Now set up waiters on my incoming stuff.
        if self.firstpass and this == self.my_id:
            for i, this_notifier in enumerate(self.peer_list[self.my_id]):
                this_notifier.num = i
                tmp = EventfdReader(this_notifier, self.ERcallback, self)
                tmp.start()

        # NOW I'm almost done.
        self.get_nodenames()
        if self.args.verbose:
            print('Setup for peer id %d is finished' % this)
        if self.firstpass:
            if self.args.verbose:
                print('Announcing myself to server ID', self.server_id)
            self.place_and_go('server', 'Ready player %s' % self.nodename)

        self.firstpass = False

    def connectionMade(self):
        if self.args.verbose:
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
        selph = vectorobj.cbdata
        nodename, msg = selph.mailbox.pickup_from_slot(vectorobj.num)
        print('%10s (%2d) -> "%s"' % (nodename, vectorobj.num, msg))
        if msg == 'ping':
            try:
                selph.place_and_go(vectorobj.num, 'pong')
            except Exception as e:
                print('pong bombed:', str(e))

    #----------------------------------------------------------------------
    # Command line parsing.  I'm just trying to get it to work.

    def doCommand(self, cmdline):
        if not cmdline.strip():
            return
        elems = cmdline.split()
        cmd = elems.pop(0)

        try:    # Errors in here break things
            if cmd in ('p', 'ping', 's', 'send'):
                if cmd.startswith('p'):
                    assert len(elems) == 1, 'Missing dest'
                    cmd = 'send'
                    elems.append('ping')    # Message payload
                else:
                    assert len(elems) >= 1, 'Missing dest'
                dest = elems.pop(0)
                msg = ' '.join(elems)       # Empty list -> empty string
                self.place_and_go(dest, msg)
                return

            if cmd in ('i', 'int'):
                assert len(elems) >= 2, 'Missing dest and/or src'
                dest = elems.pop(0)
                src = elems.pop(0)
                msg = ' '.join(elems)       # Empty list -> empty string
                self.place_and_go(dest, msg, src)
                return

            if cmd in ('d', 'dump'):
                print('Actor event fds:')
                for key in sorted(self.fd_list.keys()):
                    print('\t%2d %s' % (key, self.fd_list[key]))

                print('\nClient node/host names:')
                for key in sorted(self.id2nodename.keys()):
                    print('\t%2d %s' % (key, self.id2nodename[key]))

                print('\nPeer list keys:')
                print('\t%s' % sorted(self.peer_list.keys()))
                return

            if cmd in ('h', 'help') or '?' in cmd:
                print('dest and src can be integer, hostname, or "server"\n')
                print('h[elp]\n\tThis message')
                print('p[ing] dest\n\tShorthand for "send dest ping"')
                print('s[end] dest [text...]\n\tLike "int", implicit src=me')

                print('\nLegacy commands from QEMU "ivshmem-client":\n')
                print('i[nt] dest src [text...]\n\tCan spoof src')

                print('\nThis ID = %2d (%s)' % (self.my_id, self.nodename))
                self.get_nodenames()
                for id, nodename in self.id2nodename.items():
                    if id == self.my_id:
                        continue
                    print('Peer ID = %2d (%s)' % (id, nodename))
                return

            print('Unrecognized command "%s", try "help"' % cmd)

        except Exception as e:
            print('Error: %s' % str(e), file=sys.stderr)

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

    def buildProtocol(self, addr):
        if self.args.verbose > 1:
            print('buildProtocol', addr.name)
        protobj = ProtocolIVSHMSGClient(self.args)
        Commander(protobj)
        return protobj

    def startedConnecting(self, connector):
        print('Started connecting')

    def clientConnectionFailed(self, connector, reason):
        print('Failed connection:', str(reason))

    def clientConnectionLost(self, connector, reason):
        print('Lost connection:', str(reason))

    def run(self):
        TIreactor.run()

if __name__ == '__main__':
    from pdb import set_trace
    set_trace()
    pass
