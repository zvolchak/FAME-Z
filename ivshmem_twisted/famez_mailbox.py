#!/usr/bin/python3

# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area split into "slots".   slot 0 is a global read-only
# area populated by the server, then "nClients" worth of slots, and a final
# entry "nClients + 1" is the server mailslot.  This is contiguous because it
# it aligns with how QEMU expects delivery of peer info, including the FAME-Z
# server (which is an extension beyond stock QEMU IVSHMSG protocol).
# Right now a mailslot is 512 bytes: 128 bytes of metadata (currently about
# 96 used), then 384 of message buffer.
# Go for the max slots in the file to hardwire libvirt domain XML file size.
# famez.ko will read the global data to understand the mailbox layout.

# All numbers are unsigned long long (8 bytes).  All strings are multiples
# of 16 (including the C terminating NULL) on 32-byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ax -c -tu8 -tx8".

import mmap
import os
import struct
import sys
from time import sleep
from time import time as NOW

from os.path import stat as STAT    # for constants
from pdb import set_trace


class FAMEZ_MailBox(object):

    # QEMU rules: file size (product of first two) must be a power of two.
    MAILBOX_SLOTSIZE = 512
    MAILBOX_MAX_SLOTS = 16    # Dummy + server leaves 14 actual clients

    G_SLOTSIZE_off = 0        # in the space of mailslot index 0
    G_MSG_OFFSET_off = 8
    G_NCLIENTS_off = 16
    G_NEVENTS_off = 24
    G_SERVER_ID_off = 32

    # Metadata (front of famez_mailslot_t) is char[32] plus a few uint64_t.
    # The actual message space starts after that, 32-byte aligned, which
    # makes this look pretty: od -Ad -c -tx8 /dev/shm/famez_mailbox.

    # Datum 1: 4 longs == up to 31 chars of NUL-terminated C string
    MS_NODENAME_off = 0
    MS_NODENAME_sz = 32

    # Datum 2: 4 longs == up to 31 chars of NUL-terminated C string
    MS_CCLASS_off = MS_NODENAME_off + MS_NODENAME_sz
    MS_CCLASS_sz = 32

    # Datum 3: 1 long
    MS_MSGLEN_off = MS_CCLASS_off + MS_CCLASS_sz

    # Datum 4: 1 long
    MS_PEER_ID_off = MS_MSGLEN_off + 8

    # Datum 5: 1 long
    MS_LAST_RESPONDER_off = MS_PEER_ID_off + 8

    # 5 longs of padding == 12 longs after 2nd char[32] and finally...

    MS_MSG_off = 128
    MS_MAX_MSGLEN = 384

    fd = None       # There can be only one
    mm = None       # Then I can access fill() from the class
    nClients = None
    nEvents = None
    server_id = None

    #-----------------------------------------------------------------------
    # mm/C "strings" are null-padded byte arrays.  Undo it.


    @staticmethod
    def bytes2str(inbytes):
        return inbytes.split(b'\0', 1)[0].decode()

    @classmethod
    def _pullstring(cls, id, offset, size):
        assert 1 <= id <= cls.server_id, 'slot is bad: %d' % id
        index = id * cls.MAILBOX_SLOTSIZE + offset
        fmt = '%ds' % size
        tmp = struct.unpack(fmt, cls.mm[index:index + size])[0]
        return cls.bytes2str(tmp)

    @classmethod
    def _pullblob(cls, index, fmt):
        return struct.unpack(fmt, cls.mm[index:index + struct.calcsize(fmt)])

    @classmethod
    def nodename(cls, id):
        return cls._pullstring(id, cls.MS_NODENAME_off, cls.MS_NODENAME_sz)

    def cclass(cls, id):
        return cls._pullstring(id, cls.MS_CCLASS_off, cls.MS_CCLASS_sz)

    #-----------------------------------------------------------------------
    # Globals at offset 0 (slot 0)
    # Each slot (1 through nClients) has a peer_id.
    # Server mailbox is always at the slot following the nClients.
    # First 32 bytes of any slot are NUL-terminated host name (a C string).


    def _initialize_mailbox(self, args):
        self.__class__.mm = mmap.mmap(self.fd, 0)       # Only done once

        # Empty it.  Simple code that's never too demanding on size (now 32k)
        data = b'\0' * self.filesize
        self.mm[0:len(data)] = data

        # Fill in the globals; used by famez.ko and the C struct famez_globals.
        data = struct.pack('QQQQQ',                         # unsigned long long
            self.MAILBOX_SLOTSIZE, self.MS_MSG_off,         # constants
            args.nClients, args.nEvents, args.server_id)    # runtime
        self.mm[0:len(data)] = data

        # Set the peer_id for each slot as a C integer.  While python client
        # can discern a sender, the famez.ko driver needs an assist.

        for slot in range(1, args.nEvents):
            index = slot * self.MAILBOX_SLOTSIZE + self.MS_PEER_ID_off
            packed_peer = struct.pack('Q', slot)   # uint64_t
            # print('------- index = %d, size =  = %d' %
                # (index, len(packed_peer)))
            self.mm[index:index + len(packed_peer)] = packed_peer

        # My "hostname", zero-padded
        data = 'Z-switch'.encode() if args.smart else 'Z-server'.encode()
        index = args.server_id * self.MAILBOX_SLOTSIZE
        self.mm[index:index + len(data)] = data

        # Shortcut rutime values in self.
        self.__class__.nClients = args.nClients
        self.__class__.nEvents = args.nEvents
        self.__class__.server_id = args.server_id

    #----------------------------------------------------------------------
    # Polymorphic.  Someday I'll learn about metaclasses.  While initialized
    # as an instance, use of the whole file and individual slots is done
    # as class methods and attributes, so __init__ is a singleton.

    _beentheredonethat = False

    def __init__(self, args=None, fd=-1, client_id=-1, nodename=None):
        '''Server: args with command line stuff from command line.
           Client: starts with an fd and id read from AF_UNIX socket.'''
        if self._beentheredonethat:
            return
        self.__class__._beetnheredonethat = True

        assert (self.MS_MSG_off + self.MS_MAX_MSGLEN
            == self.MAILBOX_SLOTSIZE), 'Fix this NOW'
        assert self.MS_LAST_RESPONDER_off + 8 <= self.MS_MSG_off, \
            'Fix this too'
        self.filesize = self.MAILBOX_MAX_SLOTS * self.MAILBOX_SLOTSIZE

        if args is None:
            assert fd >= 0 and client_id > 0 and isinstance(nodename, str), \
                'Bad call, ump!'
            self.__class__.fd = fd
            self.nodename = nodename
            self._init_mailslot(client_id, nodename)
            return
        assert fd == -1 and client_id == -1, 'Cannot assign ids to server'

        path = args.mailbox     # Match previously written code
        gr_gid = -1     # Makes no change.  Try Debian, CentOS, other
        for gr_name in ('libvirt-qemu', 'libvirt', 'libvirtd'):
            try:
                gr_gid = grp.getgrnam(gr_name).gr_gid
                break
            except Exception as e:
                pass

        if '/' not in path:
            path = '/dev/shm/' + path
        oldumask = os.umask(0)
        try:
            if not os.path.isfile(path):
                fd = os.open(path, os.O_RDWR | os.O_CREAT, mode=0o666)
                os.posix_fallocate(fd, 0, self.filesize)
                os.fchown(fd, -1, gr_gid)
            else:   # Re-condition and re-use
                lstat = os.lstat(path)
                assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
                assert lstat.st_size >= self.filesize, \
                    'existing size (%d) is < required (%d)' % (
                        lstat.st_size, self.filesize)
                if lstat.st_gid != gr_gid and gr_gid > 0:
                    print('Changing %s to group %s' % (path, gr_name))
                    os.chown(path, -1, gr_gid)
                if lstat.st_mode & 0o660 != 0o660:  # at least
                    print('Changing %s to permissions 666' % path)
                    os.chmod(path, 0o666)
                fd = os.open(path, os.O_RDWR)
        except Exception as e:
            raise RuntimeError('Problem with %s: %s' % (path, str(e)))

        os.umask(oldumask)

        self.path = path                        # Final absolute path
        self.__class__.fd = fd
        self._initialize_mailbox(args)

    #----------------------------------------------------------------------
    # Dig the mail and node name out of the slot for peer_id (1:1 mapping).
    # It's not so much (passively) receivng mail as it is actively getting.

    @classmethod
    def retrieve(cls, peer_id, asbytes=False, clear=True):
        '''Return the nodename and message.'''
        assert 1 <= peer_id <= cls.server_id, \
            'Slotnum is out of domain 1 - %d' % (cls.server_id)
        index = peer_id * cls.MAILBOX_SLOTSIZE     # start of nodename
        nodename, cclass, msglen = cls._pullblob(index, '32s32sQ')
        nodename = cls.bytes2str(nodename)
        cclass = cls.bytes2str(cclass)
        index += cls.MS_MSG_off
        fmt = '%ds' % msglen
        msg = cls._pullblob(index, fmt)[0]

        # The message is copied so mark the mailslot length zero as handshake
        # to the requester that its mailbox has been emptied.

        if clear:
            index = peer_id * cls.MAILBOX_SLOTSIZE + cls.MS_MSGLEN_off
            cls.mm[index:index + 8] = struct.pack('Q', 0)

        # Clean up the message copyout, which is a single element tuple
        # that has a NUL at msglen.
        msg = msg[:msglen]
        if not asbytes:
            msg = msg.decode()
        return nodename, msg

    #----------------------------------------------------------------------
    # Post a message to the indicated mailbox slot but don't kick the
    # EventFD.  First, this routine doesn't know about them and second,
    # keeping it a separate operation facilitates sender spoofing.

    @classmethod
    def fill(cls, sender_id, msg):
        assert 1 <= sender_id <= cls.server_id, \
            'Peer ID is out of domain 1 - %d' % (cls.server_id)
        if isinstance(msg, str):
            msg = msg.encode()
        assert isinstance(msg, bytes), 'msg must be string or bytes'
        msglen = len(msg)   # It's bytes now
        assert msglen < cls.MS_MAX_MSGLEN, 'Message too long'

        # The previous responder needs to clear the msglen to indicate it
        # has pulled the message out of the sender's mailbox.
        index = sender_id * cls.MAILBOX_SLOTSIZE + cls.MS_MSGLEN_off
        stop = NOW() + 1.05
        while NOW() < stop and cls._pullblob(index, 'Q')[0]:
            sleep(0.1)
        if NOW() >= stop:
            print('pseudo-HW not ready to receive timeout: now stomping')

        cls.mm[index:index + 8] = struct.pack('Q', msglen)
        index = sender_id * cls.MAILBOX_SLOTSIZE + cls.MS_MSG_off
        cls.mm[index:index + msglen] = msg
        cls.mm[index + msglen] = 0     # NUL-terminate the message.

    #----------------------------------------------------------------------
    # Called by Python client on graceful shutdowns, and always by server
    # when a peer dies.  This is mostly for QEMU crashes so the nodename
    # is not reused when a QEMU restarts, before loading famez.ko.

    @classmethod
    def clear_mailslot(cls, id, nodenamebytes=None):
        assert 1 <= id <= cls.server_id, 'slot is bad: %d' % id
        index = id * cls.MAILBOX_SLOTSIZE
        zeros = b'\0' * (cls.MS_NODENAME_sz + cls.MS_CCLASS_sz)
        cls.mm[index:index + len(zeros)] = zeros
        if nodenamebytes:
            assert len(nodenamebytes) < cls.MS_NODENAME_sz
            cls.mm[index:index + len(nodenamebytes)] = nodenamebytes

    #----------------------------------------------------------------------
    # Called only by client.  mmap() the file, set hostname.  Many of the
    # parameters must be retrieved from the globals area of the mailbox.

    @classmethod
    def _init_mailslot(cls, id, nodename):
        buf = os.fstat(cls.fd)
        assert STAT.S_ISREG(buf.st_mode), 'Mailbox FD is not a regular file'
        if cls.mm is None:
            cls.mm = mmap.mmap(cls.fd, 0)
            (cls.nClients,
             cls.nEvents,
             cls.server_id) = cls._pullblob(cls.G_NCLIENTS_off, 'QQQ')

        if id > cls.server_id:  # Probably a test run
            return

        # mailbox slot starts with nodename
        cls.clear_mailslot(id, nodenamebytes=nodename.encode())
