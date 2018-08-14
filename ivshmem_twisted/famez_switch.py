#!/usr/bin/python3

# Running in the "context" of famez_server.  Message has already been
# retrieved and needs parsing and a response generated.

import os
import sys

from pdb import set_trace

###########################################################################
# Create a subroutine name out of the elements passed in.  Return
# the remainder.  Start with the least-specific construct.


def _unprocessed(client, *args, **kwargs):
    return False
    cmdlineargs = getattr(client, 'args', client.SI.args)
    SI = getattr(client, 'SI', False)
    if cmdlineargs.verbose and SI:
        SI.logmsg('NOOP', args, kwargs)
    return False


def chelsea(elements):
    entry = ''          # They begin with a leading '_', wait for it...
    G = globals()
    for i, e in enumerate(elements):
        e = e.replace('-', '_')         # Such as 'Link CTL Peer-Attribute'
        entry += '_%s' % e
        # print('Looking for %s()...' % entry, end='', file=sys.stderr)
        if entry in G:
            # print('found it', file=sys.stderr)
            return G[entry], elements[i + 1:]
        # print('NOPE', file=sys.stderr)
    return _unprocessed, elements

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
# Might belong in famez_mailbox


def _send_response(mailbox, vectors, response, from_id):
    mailbox.fill(from_id, response)
    vectors[from_id].incr()
    return True     # FIXME: is there anything to detect?


def client_responds(peer, response):
    return _send_response(peer.mailbox, peer.xyzzy, response, peer.id)


def server_responds(peer, response):
    return _send_response(peer.SI.mailbox, peer.vectors, response, peer.SI.server_id)

###########################################################################
# Gen-Z 1.0 "11.6 Link RFC"


def _link_rfc(client, subelements):
    if not client.SI.args.smart:
        client.SI.logmsg('This switch is not a manager')
        return False
    kv = CSV2dict(subelements[0])
    try:
        uS = int(kv['ttcus'])
    except KeyError as e:
        client.SI.logmsg('%d: Link RFC missing TTCuS' % client.id)
        return False
    except (TypeError, ValueError) as e:
        uS = 999999
    if uS > 1000:  # 1 ms, about the cycle time of this server
        client.SI.logmsg('Delay == %duS, dropping request' % uS)
        return False
    client.SID = client.SI.defaultSID
    client.CID = client.id * 100
    response = 'CtrlWrite Space=0,SID=%d,CID=%d' % (client.SID, client.CID)
    return server_responds(client, response)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL"
# send_LinkACK is also used from other modules.


def send_LinkACK(client, details, fromServer=True, nack=False):
    from_id = client.SI.server_id if fromServer else client.id
    if nack:
        response = 'Link Ctl NAK'
    else:
        response = 'Link Ctl ACK %s' % details
    if fromServer:
        return server_responds(client, response)
    return cient_responds(client, response)


def _link_ctl_peer_attribute(client, subelements):
    '''Subelements should be empty but won't be checked.'''
    details = 'C-Class=Switch,SID0=%d,CID0=%d' % (
        client.SI.server_SID0, client.SI.server_CID0)
    return send_LinkACK(client, details)

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.


def switch_handler(client, request):
    '''Return True if successfully parsed and processed.'''
    elements = request.lower().split()
    try:
        handler, subelements = chelsea(elements)
        return handler(client, subelements)
    except Exception as e:
        print(str(e), file=sys.stderr)
        pass
    return False

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
