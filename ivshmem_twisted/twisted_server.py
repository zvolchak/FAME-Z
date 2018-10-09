#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import functools
import grp
import mmap
import os
import random
import struct
import sys
import time

from collections import OrderedDict
from pprint import pprint

# While deprecated, it has the best examples and is the only thing I
# could get working.  twisted.logger.Logger() is the new way.
from twisted.python import log as TPlog
from twisted.python.logfile import DailyLogFile

from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXServerEndpoint

from twisted.internet.protocol import ServerFactory as TIPServerFactory
from twisted.internet.protocol import Protocol as TIPProtocol

try:
    from commander import Commander
    from famez_mailbox import FAMEZ_MailBox
    from famez_requests import handle_request, send_payload
    from general import ServerInvariant
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from ivshmem_sendrecv import ivshmem_send_one_msg
    from twisted_restapi import MailBoxReSTAPI
except ImportError as e:
    from .commander import Commander
    from .famez_mailbox import FAMEZ_MailBox
    from .famez_requests import handle_request, send_payload
    from .general import ServerInvariant
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from .ivshmem_sendrecv import ivshmem_send_one_msg
    from .twisted_restapi import MailBoxReSTAPI

# Don't use peer ID 0, certain docs imply it's reserved.  Put the clients
# from 1 - nClients, and the server goes at nClients + 1.  Then use slot
# 0 as global data storage, primarily the server command-line arguments.

IVSHMEM_UNUSED_ID = 0

PRINT = functools.partial(print, file=sys.stderr)
PPRINT = functools.partial(pprint, stream=sys.stderr)

###########################################################################
# Broken; need to get smarter to find actual module loggers.
# See the section below with "/dev/null" with the bandaid fix.

import logging

def shutdown_http_logging():
    for module in ('urllib3', 'requests', 'asdfjkl'):
        logger = logging.getLogger(module)
        logger.addHandler(logging.NullHandler())
        logger.setLevel(logging.CRITICAL)
        logger.disabled = True
        logger.propagate = False
        # print(vars(logger), file=sys.stderr)

###########################################################################
# See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
# qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
# qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
# qemu/contrib/ivshmem-client.c::ivshmem_client_connect()


class ProtocolIVSHMSGServer(TIPProtocol):

    SERVER_IVSHMEM_PROTOCOL_VERSION = 0

    SI = None
    responder_id = None     # ie, I am the one responding to interrupts

    def __init__(self, factory):
        '''"self" is a new client connection, not "me" the server.'''
        shutdown_http_logging()
        if self.SI is None:
            assert isinstance(factory, TIPServerFactory), 'arg0 not my Factory'
            self.__class__.SI = ServerInvariant(factory.cmdlineargs)
            SI = self.SI
            self.__class__.responder_id = SI.server_id  # THE MARK OF THE BEAST

            # Non-standard addition to IVSHMEM server role: this server can be
            # interrupted and messaged to particpate in client activity.
            # This variable will get looped even if it's empty (silent mode).
            SI.EN_list = []

            # Usually create eventfds for receiving messages in IVSHMSG and
            # set up a callback.  This early arming is not a race condition
            # as the peer for which this is destined has not yet been told
            # of the fds it would use to trigger here.

            if not factory.cmdlineargs.silent:
                SI.EN_list = ivshmem_event_notifier_list(SI.nEvents)
                # The actual client doing the sending needs to be fished out
                # via its "num" vector.
                for i, EN in enumerate(SI.EN_list):
                    EN.num = i
                    tmp = EventfdReader(EN, self.ServerCallback, SI)
                    if i:   # Technically it blocks mailslot 0, the globals
                        tmp.start()

        self.create_new_peer_id()

        # Default since QEMU VM might not load drivers at outset.  Debugger
        # client will quickly overwrite this.
        self.peerattrs = {
            'CID0': '0',
            'SID0': '0',
            'C-Class': 'Driverless QEMU'
        }
        # A first connection is raw QEMU which doesn't speak for itself.
        FAMEZ_MailBox.slots[self.id].cclass = 'Driverless QEMU'

    @property
    def promptname(self):
        '''For Commander prompt'''
        return 'Z-switch' if self.SI.args.smart else 'Z-server'

    def logPrefix(self):    # This override works after instantiation
        return 'ProtoIVSHMSG'

    def dataReceived(self, data):
        ''' TNSH :-) '''
        self.SI.logmsg('dataReceived, quite unexpectedly')
        raise NotImplementedError(self)

    # If errors occur early enough, send a bad revision to the client so it
    # terminates the connection.  Remember, "self" is a proxy for a peer.
    def connectionMade(self):
        recycled = self.SI.recycled.get(self.id, None)
        if recycled:
            del self.SI.recycled[recycled.id]
        msg = 'new socket %d == peer id %d %s' % (
              self.transport.fileno(), self.id,
              'recycled' if recycled else ''
        )
        self.SI.logmsg(msg)
        if self.id == -1:           # set from __init__
            self.SI.logmsg('Max clients reached')
            self.send_initial_info(False)   # client complains but with grace
            return

        # The original original code was written around this variable name.
        # Keep that convention for easier comparison.
        server_peer_list = list(self.SI.clients.values())

        # Server line 175: create specified number of eventfds.  These are
        # shared with all other clients who use them to signal each other.
        # Recycling keeps QEMU sessions from dying when other clients drop,
        # a perk not found in original code.
        if recycled:
            self.EN_list = recycled.EN_list
        else:
            try:
                self.EN_list = ivshmem_event_notifier_list(self.SI.nEvents)
            except Exception as e:
                self.SI.logmsg('Event notifiers failed: %s' % str(e))
                self.send_initial_info(False)
                return

        # Server line 183: send version, peer id, shm fd
        if self.SI.args.verbose:
            PRINT('Sending initial info to new peer...')
        if not self.send_initial_info():
            self.SI.logmsg('Send initial info failed')
            return

        # Server line 189: advertise the new peer to others.  Note that
        # this new peer has not yet been added to the list; this loop is
        # NOT traversed for the first peer to connect.
        if not recycled:
            if self.SI.args.verbose:
                PRINT('NOT recycled: advertising other peers...')
            for other_peer in server_peer_list:
                for peer_EN in self.EN_list:
                    ivshmem_send_one_msg(
                        other_peer.transport.socket,
                        self.id,
                        peer_EN.wfd)

        # Server line 197: advertise the other peers to the new one.
        # Remember "this" new peer proxy has not been added to the list yet.
        if self.SI.args.verbose:
            PRINT('Advertising other peers to the new peer...')
        for other_peer in server_peer_list:
            for other_peer_EN in other_peer.EN_list:
                ivshmem_send_one_msg(
                    self.transport.socket,
                    other_peer.id,
                    other_peer_EN.wfd)

        # Non-standard voodoo extension to previous advertisment: advertise
        # this server to the new peer.  To QEMU it just looks like one more
        # grouping in the previous batch.  Exists only in non-silent mode.
        if self.SI.args.verbose:
            PRINT('Advertising this server to the new peer...')
        for server_EN in self.SI.EN_list:
            ivshmem_send_one_msg(
                self.transport.socket,
                self.SI.server_id,
                server_EN.wfd)

        # Server line 205: advertise the new peer to itself, ie, send the
        # eventfds it needs for receiving messages.  This final batch
        # where the embedded self.id matches the initial_info id is the
        # sentinel that communications are finished.
        if self.SI.args.verbose:
            PRINT('Advertising the new peer to itself...')
        for peer_EN in self.EN_list:
            ivshmem_send_one_msg(
                self.transport.socket,
                self.id,
                peer_EN.get_fd())   # Must be a good story here...

        # And now that it's finished:
        self.SI.clients[self.id] = self

        # QEMU did the connect but its VM is probably not running well
        # enough to respond.  Since there's no (easy) way to tell,
        # this is a blind shot...
        self.printswitch(self.SI.clients, 0)    # Driverless QEMU or Debugger
        if self.SI.args.smart:
            send_payload(self, 'Link CTL Peer-Attribute')

    def connectionLost(self, reason):
        '''Tell the other peers that this one has died.'''
        if reason.check(TIError.ConnectionDone) is None:    # Dirty
            txt = 'Dirty'
        else:
            txt = 'Clean'
        self.SI.logmsg('%s disconnect from peer id %d' % (txt, self.id))
        # For QEMU crashes and shutdowns.  Not the VM, but QEMU itself.
        FAMEZ_MailBox.clear_mailslot(self.id)

        if self.id in self.SI.clients:     # Only if everything was completed
            del self.SI.clients[self.id]
        if self.SI.args.recycle:
            self.SI.recycled[self.id] = self
        else:
            try:
                for other_peer in self.SI.clients.values():
                    ivshmem_send_one_msg(other_peer.transport.socket, self.id)

                for EN in self.EN_list:
                    EN.cleanup()
            except Exception as e:
                self.SI.logmsg('Closing peer transports failed: %s' % str(e))
        self.printswitch(self.SI.clients)

    def create_new_peer_id(self):
        '''Determine the lowest unused client ID and set self.id.'''

        self.SID0 = 0   # When queried, the answer is in the context...
        self.CID0 = 0   # ...of the server/switch, NOT the proxy item.
        if len(self.SI.clients) >= self.SI.nClients:
            self.id = -1    # sentinel
            return  # Until a Link RFC is executed

        # dumb: monotonic from 1; smart: random (finds holes in the code).
        # Generate ID sets used by each.
        active_ids = frozenset(self.SI.clients.keys())
        unused_ids = frozenset((range(self.SI.nClients + 2))) - \
                     frozenset((IVSHMEM_UNUSED_ID, self.SI.server_id))
        available_ids = unused_ids - active_ids
        if self.SI.args.smart:
            self.id = random.choice(tuple(available_ids))
        else:
            if not self.SI.clients:   # empty
                self.id = 1
            else:
                self.id = (sorted(available_ids))[0]

        # FIXME: This should have been set up before socket connection made?
        self.nodename = FAMEZ_MailBox.slots[self.id].nodename
        self.cclass = FAMEZ_MailBox.slots[self.id].cclass
        if self.SI.args.smart:
            self.SID0 = self.SI.default_SID
            self.CID0 = self.id * 100

    def send_initial_info(self, ok=True):
        thesocket = self.transport.socket   # self is a proxy for the peer.
        try:
            # 1. Protocol version without fd.
            if not ok:  # Violate the version check and bomb the client.
                PRINT('Early termination')
                ivshmem_send_one_msg(thesocket, -1)
                self.transport.loseConnection()
                self.id = -1
                return
            if not ivshmem_send_one_msg(thesocket,
                self.SERVER_IVSHMEM_PROTOCOL_VERSION):
                PRINT('This is screwed')
                return False

            # 2. The client's (new) id, without an fd.
            ivshmem_send_one_msg(thesocket, self.id)

            # 3. -1 for data with the fd of the ivshmem file.  Using this
            # protocol a valid fd is required.
            ivshmem_send_one_msg(thesocket, -1, FAMEZ_MailBox.fd)
            return True
        except Exception as e:
            PRINT(str(e))
        return False

    # Match the signature of twisted_client object so they're both compliant
    # with downstream processing.  General lookup form is [dest][src], ie,
    # first get the list for dest, then pick out src ("from") trigger EN.
    @property
    def responder_EN(self):
        return self.EN_list[self.responder_id]  # requester not used

    # The cbdata is a class variable common to all requester proxy objects.
    # The object which serves as the responder needs to be calculated.
    @staticmethod
    def ServerCallback(vectorobj):
        requester_id = vectorobj.num
        requester_name = FAMEZ_MailBox.slots[requester_id].nodename
        request = FAMEZ_MailBox.retrieve(requester_id)
        SI = vectorobj.cbdata

        # The requester can die between its request and this callback.
        try:
            responder = SI.clients[requester_id]
            responder.requester_id = requester_id   # Pedantic?
            # For QEMU/VM, this may be the first chance to grab this (if
            # the drivers hadn't come up before).
            if not responder.nodename:
                responder.nodename = requester_name
                responder.cclass = FAMEZ_MailBox.slots[requester_id].cclass
        except KeyError as e:
            SI.logmsg('Disappeering act by %d' % requester_id)
            return

        ret = handle_request(request, requester_name, responder)

        # ret is either True, False, or...

        if ret == 'dump':
            # Might be some other stuff, but finally
            ProtocolIVSHMSGServer.printswitch(SI.clients)

    #----------------------------------------------------------------------
    # ASCII art switch:  Left side and right sider are each half of the ports.

    @staticmethod
    def printswitch(clients, delay=0.5):
        if int(delay) < 0 or int(delay) > 2:
            delay = 1.0
        time.sleep(delay)
        lfmt = '%s %s [%s,%s]'
        rfmt = '[%s,%s] %s %s'
        half = (FAMEZ_MailBox.MAILBOX_MAX_SLOTS - 1) // 2
        NSP = 32
        lspaces = ' ' * NSP
        PRINT('\n%s  _________' % lspaces)
        for i in range(1, half + 1):
            left = i
            right = left + half
            try:
                ldesc = lspaces
                c = clients[left]
                pa = c.peerattrs
                ldesc += lfmt % (c.cclass, c.nodename, pa['CID0'], pa['SID0'])
            except KeyError as e:
                pass
            try:
                c = clients[right]
                pa = c.peerattrs
                rdesc = rfmt % (pa['CID0'], pa['SID0'], c.cclass, c.nodename)
            except KeyError as e:
                rdesc = ''
            PRINT('%-s -|%1d    %2d|- %s' % (ldesc[-NSP:], left, right, rdesc))
        PRINT('%s  =========' % lspaces)

    #----------------------------------------------------------------------
    # Command line parsing, picked up by commander.py

    def doCommand(self, cmd, args=None):

        if cmd in ('h', 'help') or '?' in cmd:
            print('h[elp]\n\tThis message')
            print('d[ump]\n\tPrint status of all ports')
            print('q[uit]\n\tShut it all down')
            return True

        if cmd in ('d', 'dump'):
            if self.SI.args.verbose > 1:
                PRINT('')
                for id, peer in self.SI.clients.items():
                    PRINT('%10s: %s' % (peer.nodename, peer.peerattrs))
                    if self.SI.args.verbose > 2:
                        PPRINT(vars(peer), stream=sys.stdout)
            self.printswitch(self.SI.clients, 0)
            return True

        if cmd in ('q', 'quit'):
            self.transport.loseConnection()
            return False

        raise NotImplementedError('asdf')

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
        args.server_id = args.nClients + 1
        args.nEvents = args.nClients + 2

        # It's a singleton so no reason to keep the instance, however it's
        # the way I wrote the Klein API server so...
        mb = FAMEZ_MailBox(args=args)
        MailBoxReSTAPI(mb)
        shutdown_http_logging()

        self.cmdlineargs = args
        if args.foreground:
            if args.verbose > 1:
                TPlog.startLogging(sys.stdout, setStdout=False)
            else:
                print('The first connection will start interactivity...')
                TPlog.startLogging(open('/dev/null', 'a'), setStdout=False)
                pass
        else:
            PRINT('Logging to %s' % args.logfile)
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
        args.logmsg('FAME-Z server @%d ready for %d clients on %s' %
            (args.server_id, args.nClients, args.socketpath))

        # https://stackoverflow.com/questions/1411281/twisted-listen-to-multiple-ports-for-multiple-processes-with-one-reactor

    def buildProtocol(self, useless_addr):
        # Unfortunately this doesn't work.  Search for /dev/null above.
        shutdown_http_logging()

        # Docs mislead, have to explicitly pass something to get persistent
        # state across protocol/transport invocations.  As there is only
        # one server object per process instantion, that's not necessary.
        protobj = ProtocolIVSHMSGServer(self)
        Commander(protobj)
        return protobj

    def run(self):
        TIreactor.run()

