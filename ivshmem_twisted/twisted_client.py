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
    from commander import Commander
    from famez_mailbox import FAMEZ_MailBox as MB
    from famez_requests import handle_request, send_payload
    from general import ServerInvariant
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
except ImportError as e:
    from .commander import Commander
    from .famez_mailbox import FAMEZ_MailBox as MB
    from .famez_requests import handle_request, send_payload
    from .general import ServerInvariant
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


@implementer(IFileDescriptorReceiver)   # Energizes fileDescriptorReceived
class ProtocolIVSHMSGClient(TIPProtocol):

    CLIENT_IVSHMEM_PROTOCOL_VERSION = 0

    SI = None
    id2fd_list = OrderedDict()     # Sent to me for each peer
    id2EN_list = OrderedDict()     # Generated from fd_list

    def __init__(self, cmdlineargs):
        try:                        # twisted causes blindness
            if self.SI is None:     # singleton class variables
                self.__class__.SI = ServerInvariant()
                self.SI.args = cmdlineargs

            self.id = None       # Until initial info; state machine key
            self.linkattrs = { 'State': 'up' }
            self.peerattrs = {}
            self.SID0 = 0
            self.CID0 = 0
            self.nodename = None
            self.cclass = None
            self.afterACK = []
            # The state machine major decisions about the semantics of blocks
            # of data have one predicate.   While technically unecessary,
            # firstpass guards against server coding errors.
            self._latest_fd = None
            self.firstpass = True
        except Exception as e:
            print('__init__() failed: %s' % str(e))
            # ...and any number of attribute references will fail soon
        # print('--------------------------- __init__ finished')

    @property   # For Commander prompt
    def promptname(self):
        # print('--------------------------- promptname')
        return self.nodename

    @staticmethod
    def parse_target(caller_id, instr):
        '''Return a list even for one item for consistency with keywords
           ALL and OTHERS.'''
        try:
            tmp = (int(instr), )
            if 1 <= tmp[0] <= MB.server_id:
                return tmp
        except TypeError as e:
            return None
        except ValueError as e:
            if instr.lower()[-6:] in ('server', 'switch'):
                return (MB.server_id,)
            active_ids = MB.active_ids()
            for id in active_ids:
                if MB.slots[id].nodename == instr:
                    return (id, )
            if instr.lower() == 'all':      # Includes caller_id
                return active_ids
            if instr.lower() == 'others':
                return active_ids.remove(caller_id)
        return None

    def place_and_go(self, dest, msg, src=None, reset_tracker=True):
        '''Yes, reset_tracker defaults to True here.'''
        dest_indices = self.parse_target(self.id, dest)
        if src is None:
            src_indices = (self.id,)
        else:
            src_indices = self.parse_target(self.id, src)
        if self.SI.args.verbose > 1:
            print('P&G dest %s=%s src %s=%s' %
                      (dest, dest_indices, src, src_indices))
        assert src_indices, 'missing or unknown source(s)'
        assert dest_indices, 'missing or unknown destination(s)'
        for S in src_indices:
            for D in dest_indices:
                if self.SI.args.verbose > 1:
                    print('P&G(%s, "%s", %s)' % (D, msg, S))
                try:
                    # self.requester_id = D
                    # self.responder_id = S
                    # First get the list for dest, then src ("from me")
                    # doorbell EN.  This repeat-loads a mailslot D times
                    # but I don't care.
                    send_payload(msg, D, self.id2EN_list[D][S],
                        reset_tracker=reset_tracker)
                except KeyError as e:
                    print('No such peer id', str(e))
                    continue
                except Exception as e:
                    print('place_and_go(%s, "%s", %s) failed: %s' %
                        (D, msg, S, str(e)))
                    return

    def fileDescriptorReceived(self, latest_fd):
        # print('--------------------------- fdReceived')
        assert self._latest_fd is None, 'Latest fd has not been consumed'
        self._latest_fd = latest_fd     # See the next property

    @property
    def nodename(self):
        return self._nodename

    @nodename.setter
    def nodename(self, name):
        self._nodename = name
        if name:
            MB.slots[self.id].nodename = name

    @property
    def cclass(self):
        return self._cclass

    @cclass.setter
    def cclass(self, name):
        self._cclass = name
        if name:
            MB.slots[self.id].cclass = name

    @property
    def latest_fd(self):
        # print('--------------------------- latest_fd')
        '''This is NOT idempotent!'''
        tmp = self._latest_fd
        self._latest_fd = None
        return tmp

    def retrieve_initial_info(self, data):
        # 3 longwords: protocol version w/o FD, my (new) ID w/o FD,
        # and then a -1 with the FD of the IVSHMEM file which is
        # delivered before this.
        # print('--------------------------- retrieve_initial_info')
        assert len(data) == 24, 'Initial data needs three quadwords'

        # Enough idiot checks.
        mailbox_fd = self.latest_fd
        version, self.id, minusone = struct.unpack('qqq', data)
        assert version == self.CLIENT_IVSHMEM_PROTOCOL_VERSION, \
            'Unxpected protocol version %d' % version
        assert minusone == -1, \
            'Expected -1 with mailbox fd, got %d' % minuseone

        # Initialize my mailbox slot.  Get other parameters from the
        # globals because the IVSHMSG protocol doesn't allow values
        # beyond the intial three.  The constructor does some work then
        # returns a few attributes pulled out of the globals, but work
        # is only actually done on the first call.  It's a singleton with
        # class variables so there's no reason to keep the instance.
        # SI is the object passed to the event callback so flesh it out.
        MB(fd=mailbox_fd, client_id=self.id)
        self.SI.nClients = MB.nClients
        self.SI.nEvents = MB.nEvents
        self.SI.server_id = MB.server_id

        # Gotta wait for initialized mailbox
        self.nodename = 'z%02d' % self.id
        self.cclass = 'Debugger'
        print('This ID = %2d (%s)' % (self.id, self.nodename))

    # Called multiple times so keep state info about previous calls.
    def dataReceived(self, data):
        # print('--------------------------- dataReceived')
        if self.id is None and self.firstpass:
            self.retrieve_initial_info(data)
            return      # But I'll be right back :-)

        # Now into the stream of <peer id><eventfd> pairs.  Unless it's
        # a single <peer id> which is a disconnect notification.
        latest_fd = self.latest_fd
        assert len(data) == 8, 'Expecting a signed long long'
        this = struct.unpack('q', data)[0]
        if self.SI.args.verbose > 1:
            print('Just got index %s, fd %s' % (this, latest_fd))
        assert this >= 0, 'Latest data is negative number'

        if latest_fd is None:   # "this" is a disconnect notification
            print('%s (%d) has left the building' %
                (MB.slots[this].nodename, this))
            for collection in (self.id2EN_list, self.id2fd_list):
                try:
                    del collection[this]
                except Exception as e:
                    pass
            return

        # Get a stream of batched integers, max batch length == nEvents
        # (the dummy slot 0, nClients, and the server).  There will be
        # one batch for each existing peer, then the server (see
        # the "voodoo" comment in twisted_server.py).  In general the
        # batch lengths could be different for each peer, but in FAME-Z
        # they're all the same.  Just shove all fds in, including mine.

        # Am I starting the last batch (eventfds for me that need notifiers?)
        if this == self.id and not MB.server_id:
            assert MB.server_id == self.prevthis, 'Then dont assign it'
        self.prevthis = this     # For corner case where I am first contact

        # Just save the eventfd now, generate objects later.
        try:
            tmp = len(self.id2fd_list[this])
            assert tmp <= MB.server_id, 'fd list is too long'
            if tmp == self.SI.nEvents:   # Beginning of client reconnect
                assert this != self.id, 'Updating MY eventfds??? off-by-one'
                raise KeyError('Forced update')
            self.id2fd_list[this].append(latest_fd)   # order matters
        except KeyError as e:
            self.id2fd_list[this] = [latest_fd, ]

        if self.SI.args.verbose > 1:
            print('fd list is now %s' % str(self.id2fd_list.keys()))
            for id, eventfds in self.id2fd_list.items():
                print(id, eventfds)

        # Do the final housekeeping after the final batch.  ASS-U-MES all
        # vector lists are the same length.  My vectors come last during
        # first pass.  During a new client join it's only their info.
        if ((self.firstpass and this != self.id) or
            (len(self.id2fd_list[this]) < self.SI.nEvents)):
            if self.SI.args.verbose > 1:
                print('This (%d) waiting for more fds...\n' % this)
            return

        if self.SI.args.verbose > 1:
            print('--------- Finish housekeeping')

        # First generate event notifiers from each fd_list for signalling
        # to other peers.
        for id in self.id2fd_list:          # For triggering message pickup
            if id not in self.id2EN_list:   # Paranoid
                self.id2EN_list[id] = ivshmem_event_notifier_list(
                    self.id2fd_list[id])

        # Now arm my incoming events and announce readiness.
        # FIXME: can I really get here more than once?
        if self.firstpass:
            if this == self.id:
                for i, N in enumerate(self.id2EN_list[self.id]):
                    N.num = i
                    tmp = EventfdReader(N, self.ClientCallback, self)
                    tmp.start()

            msg = 'Ready player %s' % self.nodename
            if self.SI.args.verbose:
                print(msg)
            self.place_and_go('server', 'Link CTL Peer-Attribute',
                reset_tracker=False)

        self.firstpass = False

    def connectionMade(self):
        if self.SI.args.verbose:
            print('Connection made on fd', self.transport.fileno())
        # print('--------------------------- connectionMade')

    def connectionLost(self, reason):
        # print('--------------------------- connectionLost')
        if reason.check(TIError.ConnectionDone) is None:    # Dirty
            print('Dirty disconnect')
        else:
            print('Clean disconnect')
        MB.clear_mailslot(self.id)  # In particular, nodename
        if TIreactor.running:
            TIreactor.stop()

    # Match the signature of twisted_server object so they're both compliant
    # with downstream processing.   General lookup form is [dest][src], ie,
    # first get the list for dest, then pick out src ("from me") trigger EN.
    @property
    def responder_EN(self):
        return self.id2EN_list[self.requester_id][self.responder_id]

    # The cbdata is precisely the object which can be used for the response.
    # In other words, it's directly "me", with "my" identity data.
    @staticmethod
    def ClientCallback(vectorobj):
        requester_id = vectorobj.num
        requester_name = MB.nodename(requester_id)
        request = MB.retrieve(requester_id)
        receiver = vectorobj.cbdata
        print('Raw Req ID = %d\n%s' % (requester_id, vars(receiver)))

        # Need to be set each time because of spoof cabability, especially
        # with destinations like "other" and "all"
        receiver.requester_id = requester_id
        receiver.responder_id = receiver.id   # Not like twisted_server.py

        handle_request(request, requester_name, receiver)

    #----------------------------------------------------------------------
    # Command line parsing.

    def doCommand(self, cmd, args):
        cmd = cmd.lower()
        if cmd in ('p', 'ping', 's', 'send'):
            if cmd.startswith('p'):
                assert len(args) == 1, 'Missing dest'
                cmd = 'send'
                args.append('ping')    # Message payload
            else:
                assert len(args) >= 1, 'Missing dest'
            dest = args.pop(0)
            msg = ' '.join(args)       # Empty list -> empty string
            self.place_and_go(dest, msg)
            return True

        if cmd in ('sp', 'spoof'):     # Like send but specify a src
            assert len(args) >= 2, 'Missing src and/or dest'
            src = args.pop(0)
            dest = args.pop(0)
            msg = ' '.join(args)   # Empty list -> empty string
            self.place_and_go(dest, msg, src)
            return True

        if cmd in ('d', 'dump'):    # Include the server
            if self.SI.args.verbose > 1:
                print('Peer list keys (%d max):' % (self.SI.nClients + 1))
                print('\t%s' % sorted(self.id2EN_list.keys()))

                print('\nActor event fds:')
                for key in sorted(self.id2fd_list.keys()):
                    print('\t%2d %s' % (key, self.id2fd_list[key]))
                print()

            print('Client node/host names:')
            for key in sorted(self.id2fd_list.keys()):
                print('\t%2d %s' % (key, MB.slots[key].nodename))

            print('\nMy CID0:SID0 = %d:%d' % (self.CID0, self.SID0))
            print('Link attributes:\n', self.linkattrs)
            print('Peer attributes:\n', self.peerattrs)

            return True

        if cmd in ('h', 'help') or '?' in cmd:
            print('dest/src can be integer, hostname, or "server"\n')
            print('h[elp]\n\tThis message')
            print('l[ink]\n\tLink commands (CTL and RFC)')
            print('p[ing] dest\n\tShorthand for "send dest ping"')
            print('q[uit]\n\tJust do it')
            print('r[fc]\n\tSend "Link RFC ..." to the server')
            print('s[end] dest [text...]\n\tSend text from this client')
            print('sp[oof] src dest [text...]\n\tLike send but fake the src')
            print('w[ho]\n\tList all peers')
            return True

        if cmd in ('w', 'who'):
            print('\nThis ID = %2d (%s)' % (self.id, self.nodename))
            for id in self.id2fd_list.keys():
                if id == self.id:
                    continue
                print('Peer ID = %2d (%s)' % (id, MB.slots[id].nodename))
            return True

        if cmd in ('l', 'link'):
            assert len(args) >= 1, 'Missing directive'
            msg = 'Link %s' % ' '.join(args)
            self.place_and_go('server', msg)
            return True

        if cmd in ('r', 'rfc'):
            msg = 'Link RFC TTC=27us'
            self.place_and_go('server', msg)
            return True

        if cmd in ('q', 'quit'):
            self.transport.loseConnection()
            return False

        print('Unrecognized command "%s", try "help"' % cmd)

        return True

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
