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

from twisted.internet.endpoints import UNIXServerEndpoint

from twisted.internet.protocol import ServerFactory as TIPServerFactory
from twisted.internet.protocol import Protocol as TIPProtocol

try:
    from ivshmem_sendrecv import ivshmem_send_one_msg
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from famez_mailbox import FAMEZ_MailBox
    from commander import Commander
except ImportError as e:
    from .ivshmem_sendrecv import ivshmem_send_one_msg
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from .famez_mailbox import FAMEZ_MailBox
    from .commander import Commander

# Don't use peer ID 0, certain docs imply it's reserved.  Put the clients
# from 1 - nClients, and the server goes at nClients + 1.

IVSHMEM_UNUSED_ID = 0

###########################################################################
# See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
# qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
# qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
# qemu/contrib/ivshmem-client.c::ivshmem_client_connect()


class ProtocolIVSHMSGServer(TIPProtocol):

    SERVER_IVSHMEM_PROTOCOL_VERSION = 0

    args = None        # Invariant across instances
    logmsg = None
    logerr = None
    peer_list = []     # Map peer IDs to list of eventfds
    recycled = {}
    nClients = None
    my_id = None
    nEvents = None

    # Non-standard addition to IVSHMEM server role: this server can be
    # interrupted and messaged to particpate in client activity.
    my_notifiers = None

    def __init__(self, factory):
        # First do class-level initialization of singletons.
        # FIXME make a small class for the sake of clarity.
        if self.args is None:
            assert isinstance(factory, TIPServerFactory), 'arg0 not my Factory'
            self.__class__.args = factory.args          # Seldom-used
            self.__class__.logmsg = self.args.logmsg    # Often-used
            self.__class__.logerr = self.args.logerr
            self.__class__.nClients = self.args.nClients
            self.__class__.my_id = self.args.nClients + 1   # I am the server
            self.__class__.nEvents = self.args.nClients + 2
            self.__class__.mailbox = self.args.mailbox
            self.__class__.SID = 27

            # Usually create eventfds for receiving messages in IVSHMSG and
            # set up a callback.  This early arming is not a race condition
            # as the peer for which this is destined has not yet been told
            # of the fds it would use to trigger here.  The server slot comes
            # after the nClients.
            if not self.args.silent and self.my_notifiers is None:
                self.__class__.my_notifiers = ivshmem_event_notifier_list(
                    self.nEvents)
                for i, this_notifier in enumerate(self.my_notifiers):
                    this_notifier.num = i
                    tmp = EventfdReader(this_notifier, self.ERcallback, self)
                    tmp.start()
        self.create_new_peer_id()   # Check if it worked after connection is up

    @property
    def nodename(self):
        '''For Commander prompt'''
        return self.mailbox.nodename

    def logPrefix(self):    # This override works after instantiation
        return 'ProtoIVSHMSG'

    def dataReceived(self, data):
        ''' TNSH :-) '''
        self.logmsg('dataReceived, quite unexpectedly')
        raise NotImplementedError(self)

    # Use variables that resemble the QEMU "ivshmem-server.c":  peer == self.
    # If errors occur early enough, send a bad revision to the client so it
    # terminates the connection.
    def connectionMade(peer):
        recycled = peer.recycled.get(peer.id, None)
        if recycled:
            del peer.recycled[recycled.id]
        msg = 'new socket %d == peer id %d %s' % (
              peer.transport.fileno(), peer.id,
              'recycled' if recycled else ''
        )
        peer.logmsg(msg)
        if peer.id == -1:           # set from __init__
            peer.logmsg('Max clients reached')
            peer.send_initial_info(False)   # client complains but with grace
            return
        peer.CID = peer.id * 100
        server_peer_list = peer.peer_list

        # Server line 175: create specified number of eventfds.  These are
        # shared with all other clients who use them to signal each other.
        if recycled:
            peer.vectors = recycled.vectors
        else:
            try:
                peer.vectors = ivshmem_event_notifier_list(peer.nEvents)
            except Exception as e:
                peer.logmsg('Event notifiers failed: %s' % str(e))
                peer.send_initial_info(False)
                return

        # Server line 183: send version, peer id, shm fd
        peer.send_initial_info()

        # Server line 189: advertise the new peer to others.  Note that
        # this new peer has not yet been added to the list; this loop is
        # NOT traversed for the first peer to connect.
        if not recycled:
            for other_peer in server_peer_list:
                for peer_vector in peer.vectors:
                    ivshmem_send_one_msg(
                        other_peer.transport.socket,
                        peer.id,
                        peer_vector.wfd)

        # Server line 197: advertise the other peers to the new one.
        for other_peer in server_peer_list:
            for other_peer_vector in other_peer.vectors:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    other_peer.id,
                    other_peer_vector.wfd)

        # Non-standard voodoo extension to previous advertisment: advertise
        # me (this server) to the new peer.  Consider it one more grouping
        # in the previous batch.
        if peer.my_notifiers:    # Exists only in non-silent mode
            for server_vector in peer.my_notifiers:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    peer.my_id,
                    server_vector.wfd)

        # Server line 205: advertise the new peer to itself, ie, send the
        # eventfds it needs for receiving messages.  This final batch
        # where the embedded peer.id matches the initial_info id is the
        # sentinel that communications are finished.
        for peer_vector in peer.vectors:
            ivshmem_send_one_msg(
                peer.transport.socket,
                peer.id,
                peer_vector.get_fd())   # Must be a good story here...

        # Oh yeah
        server_peer_list.append(peer)

    def connectionLost(self, reason):
        '''Tell the other peers that this one has died.'''
        if reason.check(TIError.ConnectionDone) is None:    # Dirty
            txt = 'Dirty'
        else:
            txt = 'Clean'
        self.logmsg('%s disconnect from peer id %d' % (txt, self.id))
        if self in self.peer_list:     # Only if everything was completed
            self.peer_list.remove(self)
        if self.args.recycle:
            self.recycled[self.id] = self
            return

        try:
            for other_peer in self.peer_list:
                ivshmem_send_one_msg(other_peer.transport.socket, self.id)

            for vector in self.vectors:
                vector.cleanup()

            # For QEMU crashes and shutdowns.  Not the VM, but QEMU itself.
            self.mailbox.clear_my_mailslot(override_id=self.id)

        except Exception as e:
            self.logmsg('Closing peer transports failed: %s' % str(e))

    def create_new_peer_id(self):
        '''Determine the lowest unused client ID and set self.id.'''
        # Does not occur often, don't sweat the performance.
        if len(self.peer_list) >= self.nClients:
            self.id = -1    # sentinel
            return
        current_ids = frozenset((p.id for p in self.peer_list))
        if not current_ids:
            self.id = 1
            return
        max_ids = frozenset((range(self.nClients + 2))) - \
                  frozenset((IVSHMEM_UNUSED_ID, self.my_id ))
        self.id = sorted(max_ids - current_ids)[0]

    def send_initial_info(peer, ok=True):   # keep the convention self=="peer"
        # Protocol version without fd.
        thesocket = peer.transport.socket
        if not ok:  # Violate the version check and bomb the client.
            ivshmem_send_one_msg(thesocket, -1)
            peer.transport.loseConnection()
            peer.id = -1
            return
        ivshmem_send_one_msg(thesocket, peer.SERVER_IVSHMEM_PROTOCOL_VERSION)

        # The client's id, without an fd.
        ivshmem_send_one_msg(thesocket, peer.id)

        # -1 for data with the fd of the ivshmem file.  Using this protocol
        # a valid fd is required.
        # FIXME does send_one message belong in mailbox.py?
        ivshmem_send_one_msg(thesocket, -1, peer.mailbox.fd)

    @staticmethod
    def ERcallback(vectorobj):
        selph = vectorobj.cbdata
        peer_id = vectorobj.num
        print('Received ER from peer %d' % peer_id, file=sys.stderr)
        nodename, msg = selph.mailbox.retrieve(peer_id)
        selph.logmsg('"%s" (%d) -> "%s"' % (nodename, peer_id, msg))

        # Find the peer in the list.  FIXME: convert to dict{} like client.
        for peer in selph.peer_list:
            if peer.id == vectorobj.num:
                break
        else:
            selph.logmsg('Disappeering act by %d' % vectorobj.num)
            return

        # Someday pass and parse the message (like the discover stuff).
        # For now just do the shortcut.
        if msg == 'ping':
            print('Sending PONG to %d from %d' %
                (peer.id, selph.my_id),
                file=sys.stderr)
            selph.mailbox.fill(selph.my_id, 'PONG')
            peer.vectors[selph.my_id].incr()
            return

        elems = msg.split(':')
        if len(elems) == 1:
            return

        if elems[0] == 'LinkRFC':
            if not elems[1].startswith('TTCuS='):
                selph.logmsg('%d: LinkRFC missing TTCuS' % peer.id)
                return
            try:
                uS = int(elems[1].split('=')[1])
            except Exception as e:
                uS = 999999
            if uS > 1000:  # 1 ms, about the cycle time of this server
                selph.logmsg('Delay == %duS, dropping request' % uS)
                return
            selph.mailbox.fill(
                selph.my_id,
                'CtrlWrite:SID=%d,CID=%d' % (selph.SID, selph.CID))
            peer.vectors[selph.my_id].incr()
            return

###########################################################################
# Normally the Endpoint and listen() call is done explicitly, interwoven
# with passing this constructor.  This approach used here hides all the
# twisted things in this module.


class FactoryIVSHMSGServer(TIPServerFactory):

    _required_arg_defaults = {
        'foreground':   True,       # Only affects logging choice in here
        'logfile':      '/tmp/ivshmem_log',
        'mailbox':      'ivshmem_mailbox',  # Will end up in /dev/shm
        'nClients':     2,
        'recycle':      False,      # Try to preserve other QEMUs
        'silent':       False,      # Does participate in eventfds/mailbox
        'socketpath':   '/tmp/ivshmem_socket',
        'verbose':      0,
    }

    def __init__(self, args=None):
        '''Args must be an object with the following attributes:
           foreground, logfile, mailbox, nClients, silent, socketpath, verbose
           Suitable defaults will be supplied.'''

        # Pass command line args to ProtocolIVSHMSG, then open logging.
        if args is None:
            args = argparse.Namespace()
        for arg, default in self._required_arg_defaults.items():
            setattr(args, arg, getattr(args, arg, default))

        # Mailbox may be sized above the requested number of clients to
        # satisfy QEMU IVSHMEM restrictions.
        args.mailbox = FAMEZ_MailBox(args.mailbox, nClients=args.nClients)
        server_id = args.nClients + 1

        self.args = args
        if args.foreground:
            TPlog.startLogging(sys.stdout, setStdout=False)
        else:
            print('Logging to %s' % args.logfile, file=sys.stderr)
            TPlog.startLogging(
                DailyLogFile.fromFullPath(args.logfile),
                setStdout=True)     # "Pass-through" explicit print() for debug
        args.logmsg = TPlog.msg
        args.logerr = TPlog.err

        # By Twisted version 18, "mode=" is deprecated and you should just
        # inherit the tacky bit from the parent directory.  wantPID creates
        # <path>.lock as a symlink to "PID".
        E = UNIXServerEndpoint(
            TIreactor,
            args.socketpath,
            mode=0o666,         # Deprecated at Twisted 18
            wantPID=True)
        E.listen(self)
        args.logmsg('FAME-Z server (id=%d) listening for up to %d clients on %s' %
            (server_id, args.nClients, args.socketpath))

        # https://stackoverflow.com/questions/1411281/twisted-listen-to-multiple-ports-for-multiple-processes-with-one-reactor

    def buildProtocol(self, useless_addr):
        # Docs mislead, have to explicitly pass something to get persistent
        # state across protocol/transport invocations.  As there is only
        # one server object per process instantion, that's not necessary.
        protobj = ProtocolIVSHMSGServer(self)
        Commander(protobj)
        return protobj

    def run(self):
        TIreactor.run()

