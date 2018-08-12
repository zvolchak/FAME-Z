#!/usr/bin/python3

# Message has already been retrieved

import os
import sys

from pdb import set_trace

###########################################################################

LINK =  'link'


def _unprocessed(*args, **kwargs):
    selph.logmsg('Dummy dummy dummy')
    return False


def chelsea(prefix, suffix):
    entry = '_%s_%s' % (prefix, suffix)
    # print('Looking for %s()' % entry, end='', file=sys.stderr)
    try:
        tmp = globals()[entry]
        # print(' found it', file=sys.stderr)
        return tmp
    except KeyError as e:
        # print(' NOPE', file=sys.stderr)
        return _unprocessed

###########################################################################


def CSV2dict(oneCSVstr):
    kv = {}
    elems = oneCSVstr.strip().split(',')
    for e in elems:
        KeV = e.strip().split('=')
        if len(KeV) != 2:
            continue
        kv[KeV[0].strip()] = KeV[1].strip()
    return kv

###########################################################################


def _send_LinkACK(selph, submsg, nack=False):
    hdr = 'Link:NACK:' if nack else 'Link:ACK'
    selph.mailbox.fill(selph.my_id, '%s:%s' % (hdr, submsg))
    selph.vectors[selph.my_id].incr()

###########################################################################
# Gen-Z 1.0 p xxxxx


def _link_rfc(selph, subelems):
    kv = CSV2dict(subelems[0])
    try:
        uS = int(kv['ttcus'])
    except KeyError as e:
        selph.logmsg('%d: Link RFC missing TTCuS' % selph.id)
        return False
    except (TypeError, ValueError) as e:
        uS = 999999
    if uS > 1000:  # 1 ms, about the cycle time of this server
        selph.logmsg('Delay == %duS, dropping request' % uS)
        return False
    selph.mailbox.fill(selph.my_id,
        'CtrlWrite:SID=%d,CID=%d' % (selph.SID, selph.CID))
    selph.vectors[selph.my_id].incr()
    return True

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.


def switch_handler(selph, nodename, msg):
    '''Return True if successfully parsed and processed.'''
    elems = msg.lower().split(':')
    try:
        cmd = elems.pop(0)
        subcmd = elems.pop(0)
        return chelsea(cmd, subcmd)(selph, elems)
    except Exception as e:
        pass
    return False

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
