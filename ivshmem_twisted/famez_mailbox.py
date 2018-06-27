#!/usr/bin/python3

# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area.  In non-silent mode it's split into "nSlots"
# slots: slot 0 is a global read-only area populated below, the final
# entry "nSlots-1" is the server mailbox, and the middle "nSlots-2" entries
# are for that many clients.  Right now a mailbox is 512 byts: 128 bytes of
# metadata for a sent message, and 384 of message buffer.  famez.ko will
# read the global data to understand the mailbox layout.

# All numbers are unsigned long long (8 bytes).  All strings are multiples
# of 16 (including the C terminating NULL) on 16 byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ad -c -tu8".

import mmap
import os
import struct

from pdb import set_trace

MAILBOX_SLOTSIZE = 512
MAILBOX_MESSAGE_OFFSET = 128    # Leaves 384 bytes for message
MAILBOX_MAX_SLOTS = 64

###########################################################################
# Globals at offset 0 (slot 0)
# Server mailbox at slot (nSlots - 1), first 32 bytes are host name.


def _populate_mailbox(fd, nSlots):
    mapped = mmap.mmap(fd, 0)

    # Empty it.  Simple from a code standpoint, never too demanding on size
    data = b'\0' * (nSlots * MAILBOX_SLOTSIZE)
    mapped[0:len(data)] = data

    # Slot size, message area start within a slot, nSlots
    data = struct.pack('QQQ', MAILBOX_SLOTSIZE, MAILBOX_MESSAGE_OFFSET, nSlots)
    mapped[0:len(data)] = data

    # My "hostname", zero-padded
    data = b'FAME-Z Server'
    index = (nSlots - 1) * MAILBOX_SLOTSIZE
    mapped[index:index + len(data)] = data

    mapped.close()

###########################################################################


def prepare_mailbox(path, nSlots=MAILBOX_MAX_SLOTS):
    '''Starts with mailbox base name, returns an fd to a populated file.'''

    size = nSlots * MAILBOX_SLOTSIZE
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
            STAT = os.path.stat         # for constants
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
    index = slotnum * 512     # start of nodename
    nodename, msglen = struct.unpack('32sQ', mailbox_mm[index:index + 40])
    nodename = nodename.split(b'\0', 1)[0].decode()
    index += 128
    fmt = '%ds' % msglen
    msg = struct.unpack(fmt, mailbox_mm[index:index + msglen])
    # Returned a single element tuple that should be NUL-padded to end.
    msg = msg[0].split(b'\0', 1)[0]
    if not asbytes:
        msg = msg.decode()
    return nodename, msg

###########################################################################


def place_in_slot(mailbox_mm, slotnum, msg):
    assert 1 <= slotnum < MAILBOX_MAX_SLOTS, 'Slotnum is out of domain'
    if isinstance(msg, str):
        msg = msg.encode()
    assert isinstance(msg, bytes), 'msg must be string or bytes'
    msglen = len(msg)   # It's bytes now
    assert msglen < 384, 'Message too long'

    index = slotnum * 512 + 32    # Start of msglen
    mailbox_mm[index:index + 8] = struct.pack('Q', msglen)
    index = slotnum * 512 + 128   # Start of msg
    mailbox_mm[index:index + msglen] = msg

###########################################################################


if __name__ == '__main__':
    fd = prepare_mailbox('/tmp/junk', 4)
    _populate_mailbox(fd, 4)
