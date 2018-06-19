#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the COPYING file in the
# top-level directory.

# Rocky Craig <rjsnoose@gmail.com>

import argparse
import grp
import os
from os.path import stat as STAT

from pdb import set_trace

from ivshmsg_twisted import FactoryIVSHMSG

def parse_cmdline():
    '''Single-letter arguments reflect the QEMU "ivshmem-server".'''
    parser = argparse.ArgumentParser(
        description='FAME-Z server files and vector counts',
        epilog='Options reflect those in the QEMU "ivshmem-server".'
    )

    parser.add_argument('-?', action='help')  # -h and --help are built in
    parser.add_argument('--foreground', '-F',
        help='Run in foreground (default is background with logging to a file',
        action='store_true'
    )
    parser.add_argument('--mailbox', '-M', metavar='<name>',
        help='Name of mailbox that exists in POSIX shared memory',
        default='ivshmsg_mailbox'
    )
    parser.add_argument('--nVectors', '-n', metavar='<integer>',
        help='Number of interrupt vectors per client (8 max)',
        default=1
    )
    parser.add_argument('--socketpath', '-S', metavar='/path/to/socket',
        help='Absolute path to UNIX domain socket (will be created)',
        default='/tmp/ivshmsg_socket'
    )
    parser.add_argument('--verbose', '-v',
        help='Specify multiple times to increase verbosity',
        default=0,
        action='count'
    )
    args = parser.parse_args()   # Default: sys.argv

    # Idiot checking.
    assert 1 <= args.nVectors <= 8, 'nVectors not in range 1-8'
    assert not '/' in args.mailbox, 'mailbox cannot have slashes'
    assert not os.path.exists(args.socketpath), \
        'Remove %s' % args.socketpath

    # Mailbox is shared common area.  Each client gets 8k, max 15
    # clients, server area starts at zero, thus 128k.
    args.mailbox = '/dev/shm/' + args.mailbox
    oldumask = os.umask(0)
    gr_name = 'libvirt-qemu'
    try:
        gid = grp.getgrnam(gr_name).gr_gid
        if os.path.isfile(args.mailbox):
            lstat = os.lstat(args.mailbox)
            assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
            assert lstat.st_size >= 131072, 'size is < 128k'
            assert lstat.st_gid == gid, 'group is not %s' % gr_name
            assert lstat.st_mode & 0o660 == 0o660, \
                'permissions must be 66x'
            args.mailbox_fd = os.open(args.mailbox, os.O_RDWR)
        else:
            args.mailbox_fd = os.open(
                args.mailbox, os.O_RDWR | os.O_CREAT, mode=0o666)   # umask oddities
            os.posix_fallocate(args.mailbox_fd, 0, 131072)
            os.fchown(args.mailbox_fd, -1, gid)
    except Exception as e:
        raise SystemExit('Problem with %s: %s' % (args.mailbox, str(e)))

    os.umask(oldumask)
    return args


if __name__ == '__main__':

    try:
        args = parse_cmdline()
    except Exception as e:
        raise SystemExit(str(e))
    server = FactoryIVSHMSG(args)
    server.run()
