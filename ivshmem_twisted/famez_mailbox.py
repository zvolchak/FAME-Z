#!/usr/bin/python3

# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area.  In non-silent mode it's split into "nSlots"
# slots: slot 0 is a global read-only area populated below, the final
# entry "nSlots-1" is the server mailbox, and the middle "nSlots-2" entries
# are for that many clients.  Right now a mailbox is 512 bytes:
# 128 bytes of metadata for a sent message
# Then 384 of message buffer.
# famez.ko will read the global data to understand the mailbox layout.

# All numbers are unsigned long long (8 bytes).  All strings are multiples
# of 16 (including the C terminating NULL) on 16 byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ad -c -tu8".

import mmap
import os
import struct

from os.path import stat as STAT     # for constants
from pdb import set_trace

MAILBOX_MAX_SLOTS = 64
MAILBOX_SLOT_SIZE = 512

GLOBALS_SLOT_SIZE_OFFSET = 0    # in the space of mailslot 0
GLOBALS_MESSAGE_OFFSET_OFFSET = 8
GLOBALS_NSLOTS_OFFSET = 16

MAILSLOT_NODENAME_OFFSET = 0
MAILSLOT_NODENAME_SIZE = 32     # NULL padded
MAILSLOT_MSGLEN_OFFSET = MAILSLOT_NODENAME_SIZE
MAILSLOT_MESSAGE_OFFSET = 128   # To the end of the slot, currently 384 bytes
MAILSLOT_MESSAGE_SIZE = 384

assert MAILSLOT_MESSAGE_OFFSET + MAILSLOT_MESSAGE_SIZE == MAILBOX_SLOT_SIZE, \
    'Fix this NOW'

###########################################################################
# Globals at offset 0 (slot 0)
# Server mailbox at slot (nSlots - 1), first 32 bytes are host name.


def _populate_mailbox(fd, nSlots):
    mapped = mmap.mmap(fd, 0)

    # Empty it.  Simple from a code standpoint, never too demanding on size
    data = b'\0' * (nSlots * MAILBOX_SLOT_SIZE)
    mapped[0:len(data)] = data

    # Slot size, message area start within a slot, nSlots
    data = struct.pack('QQQ', MAILBOX_SLOT_SIZE, MAILSLOT_MESSAGE_OFFSET, nSlots)
    mapped[0:len(data)] = data

    # My "hostname", zero-padded
    data = b'FAME-Z Server'
    index = (nSlots - 1) * MAILBOX_SLOT_SIZE
    mapped[index:index + len(data)] = data

    mapped.close()

###########################################################################


def prepare_mailbox(path, nSlots=MAILBOX_MAX_SLOTS):
    '''Starts with mailbox base name, returns an fd to a populated file.'''

    size = nSlots * MAILBOX_SLOT_SIZE
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
                'existing size (%d) is < required (%d)' % (lstat.st_size, size)
            if lstat.st_gid != gr_gid and gr_gid > 0:
                print('Changing %s to group %s' % (path, gr_name))
                os.chown(path, -1, gr_gid)
            if lstat.st_mode & 0o660 != 0o660:  # at least
                print('Changing %s to permissions 666' % path)
                os.chmod(path, 0o666)
            fd = os.open(path, os.O_RDWR)
    except Exception as e:
        raise SystemExit('Problem with %s: %s' % (path, str(e)))

    os.umask(oldumask)
    _populate_mailbox(fd, nSlots)
    return fd

###########################################################################


def pickup_from_slot(mailbox_mm, slotnum, asbytes=False):
    assert 1 <= slotnum < MAILBOX_MAX_SLOTS, 'Slotnum is out of domain'
    index = slotnum * MAILBOX_SLOT_SIZE     # start of nodename
    nodename, msglen = struct.unpack('32sQ', mailbox_mm[index:index + 40])
    nodename = nodename.split(b'\0', 1)[0].decode()
    index += MAILSLOT_MESSAGE_OFFSET
    fmt = '%ds' % msglen
    msg = struct.unpack(fmt, mailbox_mm[index:index + msglen])
    # Returned a single element tuple that should be NUL-padded to end.
    msg = msg[0].split(b'\0', 1)[0]
    if not asbytes:
        msg = msg.decode()
    return nodename, msg

###########################################################################


def place_in_slot(mailbox_mm, slotnum, msg):
    assert 1 <= slotnum < MAILBOX_MAX_SLOTS, 'Slotnum is way out of domain'
    if isinstance(msg, str):
        msg = msg.encode()
    assert isinstance(msg, bytes), 'msg must be string or bytes'
    msglen = len(msg)   # It's bytes now
    assert msglen < MAILSLOT_MESSAGE_SIZE, 'Message too long'

    index = slotnum * MAILBOX_SLOT_SIZE + MAILSLOT_MSGLEN_OFFSET
    mailbox_mm[index:index + 8] = struct.pack('Q', msglen)
    index = slotnum * MAILBOX_SLOT_SIZE + MAILSLOT_MESSAGE_OFFSET
    mailbox_mm[index:index + msglen] = msg
    mailbox_mm[index + msglen] = 0	# The kernel will appreciate this :-)

###########################################################################
# Called only by client.  mmap() the file, set hostname, return some globals.
# Polymorphic.


def init_mailslot(mailbox_XX, slotnum, nodename):
    if not isinstance(mailbox_XX, int):
        mailbox_mm = mailbox_XX
    else:
        buf = os.fstat(mailbox_XX)
        assert STAT.S_ISREG(buf.st_mode), 'Mailbox FD is not a regular file'
        assert 1 <= slotnum <= buf.st_size // MAILBOX_SLOT_SIZE, \
            'Slotnum %d is out of range' % slotnum
        mailbox_mm = mmap.mmap(mailbox_XX, 0)
    nSlots = struct.unpack(
        'Q',
        mailbox_mm[GLOBALS_NSLOTS_OFFSET:GLOBALS_NSLOTS_OFFSET + 8])[0]
    index = slotnum * MAILBOX_SLOT_SIZE    # mailbox slot starts with nodename
    zeros = b'\0' * MAILSLOT_NODENAME_SIZE
    mailbox_mm[index:index + len(zeros)] = zeros
    tmp = nodename.encode()
    mailbox_mm[index:index + len(tmp)] = tmp
    return mailbox_mm, nSlots

###########################################################################


if __name__ == '__main__':
    fd = prepare_mailbox('/tmp/junk', 4)
    _populate_mailbox(fd, 4)
