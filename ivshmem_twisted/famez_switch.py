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
# Not clear if this can be refactored for more general use and moved.


def _send_response(peer, selph, response):
    selph.mailbox.fill(selph.my_id, response)
    peer.vectors[selph.my_id].incr()
    return True     # FIXME: is there anything to detect?

###########################################################################


def _send_LinkACK(peer, selph, submsg, nack=False):
    return _send_response(peer, selph, 'Link:NACK:' if nack else 'Link:ACK')

###########################################################################
# Gen-Z 1.0 p xxxxx


def _link_rfc(selph, subelems, peer):
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
    return _send_response(peer, selph,
        'CtrlWrite:SID=%d,CID=%d' % (peer.SID, peer.CID))

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.


def switch_handler(selph, msgtoselph, peer):
    '''Return True if successfully parsed and processed.'''
    elems = msgtoselph.lower().split(':')
    try:
        cmd = elems.pop(0)
        subcmd = elems.pop(0)
        return chelsea(cmd, subcmd)(selph, elems, peer)
    except Exception as e:
        pass
    return False

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
