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
from time import sleep
from time import time as NOW

from os.path import stat as STAT    # for constants
from pdb import set_trace

class FAMEZ_MailBox(object):

    # QEMU rules: file size (product of first two) must be a power of two.
    MAILBOX_SLOTSIZE = 512
    MAILBOX_MAX_SLOTS = 64    # Dummy + server leaves 62 actual clients

    G_SLOTSIZE_off = 0        # in the space of mailslot index 0
    G_MSG_OFFSET_off = 8
    G_NCLIENTS_off = 16

    # Metadata (front of famez_mailslot_t) is char[32] plus a few uint64_t.
    # The actual message space starts after that, 32-byte aligned, which
    # makes this look pretty: od -Ad -c -tx8 /dev/shm/famez_mailbox.

    # Datum 1: 4 longs == up to 31 chars of NUL-terminated C string
    MS_NODENAME_off = 0
    MS_NODENAME_SIZE = 32

    # Datum 2: 1 long
    MS_MSGLEN_off = MS_NODENAME_SIZE
    MS_PEER_ID_off = MS_MSGLEN_off + 8

    # Datum 3: 1 long
    MS_LAST_RESPONDER_off = MS_PEER_ID_off + 8

    # 10 longs of padding and finally...

    MS_MSG_off = 128
    MS_MAX_MSGLEN = 384

    #-----------------------------------------------------------------------
    # Globals at offset 0 (slot 0)
    # Each slot (1 through nClients) has a peer_id.
    # Server mailbox is always at the slot following the nClients.
    # First 32 bytes of any slot are NUL-terminated host name (a C string).


    def _populate(self):
        self.mm = mmap.mmap(self.fd, 0)

        # Empty it.  Simple code that's never too demanding on size (now 32k)
        data = b'\0' * (self.MAILBOX_MAX_SLOTS * self.MAILBOX_SLOTSIZE)
        self.mm[0:len(data)] = data

        # Fill in the globals.  Must match the C struct in famez.ko.
        data = struct.pack('QQQ',
            self.MAILBOX_SLOTSIZE, self.MS_MSG_off, self.nClients)
        self.mm[0:len(data)] = data

        # Set the peer_id for each slot as a C integer.  While python client
        # can discern a sender, the famez.ko driver needs an assist.

        for slot in range(1, self.nClients + 2):
            index = slot * self.MAILBOX_SLOTSIZE + self.MS_PEER_ID_off
            packed_peer = struct.pack('Q', slot)   # uint64_t
            # print('------- index = %d, size =  = %d' %
                # (index, len(packed_peer)))
            self.mm[index:index + len(packed_peer)] = packed_peer

        # My "hostname", zero-padded
        data = self.nodename.encode()
        index = self.server_id * self.MAILBOX_SLOTSIZE
        self.mm[index:index + len(data)] = data

    #----------------------------------------------------------------------
    # Polymorphic.  Someday I'll learn about metaclasses.

    def __init__(self, pathORfd, nClients=None, client_id=-1, nodename=None):
        '''Server: Starts with string and slot count from command line.
           Client: starts with an fd and id.'''

        assert (self.MS_MSG_off + self.MS_MAX_MSGLEN
            == self.MAILBOX_SLOTSIZE), 'Fix this NOW'

        if isinstance(pathORfd, int):
            assert client_id > 0 and nodename is not None, 'Bad call, ump!'
            self.fd = pathORfd
            self.my_id = client_id
            self.nodename = nodename
            self._init_client_mailslot()
            return
        assert isinstance(pathORfd, str), 'Need a path OR open fd'
        assert client_id == -1, 'Cannot assign client id to server'
        path = pathORfd     # Match previously written code

        # Add two more slots for the globals and the server.
        assert 1 <= nClients <= self.MAILBOX_MAX_SLOTS - 2, \
            'Bad nClients (valid: 2 - %d)' % (self.MAILBOX_MAX_SLOTS - 2)

        size = self.MAILBOX_MAX_SLOTS * self.MAILBOX_SLOTSIZE
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
                os.posix_fallocate(fd, 0, size)
                os.fchown(fd, -1, gr_gid)
            else:   # Re-condition and re-use
                lstat = os.lstat(path)
                assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
                assert lstat.st_size >= size, \
                    'existing size (%d) is < required (%d)' % (
                        lstat.st_size, size)
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

        # Probably very handy to keep around.
        self.nClients = nClients
        self.path = path                        # Final absolute path
        self.fd = fd

        # Finally do "my" stuff
        self.server_id = nClients + 1           # New rule
        self.nodename = 'Z-Server'
        self._populate()

    #----------------------------------------------------------------------
    # Dig the mail and node name out of the slot for peer_id (1:1 mapping).
    # It's not so much (passively) receivng mail as it is actively getting.

    def retrieve(self, peer_id, asbytes=False, clear=True):
        '''Return the nodename and message.'''
        assert 1 <= peer_id <= self.server_id, \
            'Slotnum is out of domain 1 - %d' % (self.server_id)
        index = peer_id * self.MAILBOX_SLOTSIZE     # start of nodename
        nodename, msglen = struct.unpack('32sQ', self.mm[index:index + 40])
        nodename = nodename.split(b'\0', 1)[0].decode()
        index += self.MS_MSG_off
        fmt = '%ds' % msglen
        msg = struct.unpack(fmt, self.mm[index:index + msglen])

        # The message is copied so mark the mailslot length zero as handshake
        # to the requester that its mailbox has been emptied.

        if clear:
            index = peer_id * self.MAILBOX_SLOTSIZE + self.MS_MSGLEN_off
            self.mm[index:index + 8] = struct.pack('Q', 0)

        # Clean up the message copyout, which is a single element tuple
        # that has a NUL at msglen.
        # msg = msg[0].split(b'\0', 1)[0] # only valid for pure stringsA
        msg = msg[0][:msglen]
        if not asbytes:
            msg = msg.decode()
        return nodename, msg

    #----------------------------------------------------------------------
    # Post a message to the indicated mailbox slot but don't kick the
    # EventFD.  First, this routine doesn't know about them and second,
    # keeping it a separate operation facilitates spoofing in the client.

    def fill(self, sender_id, msg):
        assert 1 <= sender_id <= self.server_id, \
            'Peer ID is out of domain 1 - %d' % (self.server_id)
        if isinstance(msg, str):
            msg = msg.encode()
        assert isinstance(msg, bytes), 'msg must be string or bytes'
        msglen = len(msg)   # It's bytes now
        assert msglen < self.MS_MAX_MSGLEN, 'Message too long'

        # The previous responder needs to clear the msglen to indicate it
        # has pulled the message out of the sender's mailbox.
        index = sender_id * self.MAILBOX_SLOTSIZE + self.MS_MSGLEN_off
        stop = NOW() + 1.05
        while NOW() < stop and struct.unpack('Q', self.mm[index:index+8])[0]:
            sleep(0.1)
        if NOW() >= stop:
            print('pseudo-HW not ready to receive timeout: now stomping')

        self.mm[index:index + 8] = struct.pack('Q', msglen)
        index = sender_id * self.MAILBOX_SLOTSIZE + self.MS_MSG_off
        self.mm[index:index + msglen] = msg
        self.mm[index + msglen] = 0     # NUL-terminate the message.

    #----------------------------------------------------------------------
    # Called by Python client on graceful shutdowns, and always by server
    # when a peer dies.  This is mostly for QEMU crashes so the nodename
    # is not reused when a QEMU restarts, before loading famez.ko.

    def clear_my_mailslot(self, nodenamebytes=None, override_id=None):
        id = override_id if override_id else self.my_id
        assert 1 <= id <= self.server_id, 'slot is bad: %d' % id
        index = id * self.MAILBOX_SLOTSIZE
        zeros = b'\0' * self.MS_NODENAME_SIZE
        self.mm[index:index + len(zeros)] = zeros
        if nodenamebytes:
            assert len(nodenamebytes) < self.MS_NODENAME_SIZE
            self.mm[index:index + len(nodenamebytes)] = nodenamebytes

    #----------------------------------------------------------------------
    # Called only by client.  mmap() the file, set hostname.  Many of the
    # parameters must be retrieved from the globals area of the mailbox.

    def _init_client_mailslot(self):
        buf = os.fstat(self.fd)
        assert STAT.S_ISREG(buf.st_mode), 'Mailbox FD is not a regular file'
        self.mm = mmap.mmap(self.fd, 0)
        self.nClients = struct.unpack(
            'Q',
            self.mm[self.G_NCLIENTS_off:self.G_NCLIENTS_off + 8]
        )[0]
        self.server_id = self.nClients + 1      # immediately after all clients

        # mailbox slot starts with nodename
        self.clear_my_mailslot(nodenamebytes=self.nodename.encode())

###########################################################################


if __name__ == '__main__':
    mbox = FAMEZ_MailBox('/tmp/junk', 4)
