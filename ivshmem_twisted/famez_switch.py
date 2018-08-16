#!/usr/bin/python3

# Common routines for both server (switch) and clients which drill down
# on messages retrieved by parsing and generating a response.

import os
import sys

from pdb import set_trace
from pprint import pprint

try:
    from famez_mailbox import FAMEZ_MailBox
except ImportError as e:
    from .famez_mailbox import FAMEZ_MailBox

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
        try:
            KeV = e.strip().split('=')
            kv[KeV[0].strip()] = KeV[1].strip()
        except Exception as e:
            continue
    return kv

###########################################################################
# Might belong in famez_mailbox
# FIXME: make mailbox.fill() a class method and import the class to reach it.


def _send_response(peer, response, from_id, from_EN):
    FAMEZ_MailBox.fill(from_id, response)
    from_EN.incr()
    return True     # FIXME: is there anything to detect?

###########################################################################
# Also called from other modules.


def send_LinkACK(CorS, details, from_id, from_EN, nack=False):
    if nack:
        response = 'Link CTL NAK %s' % details
    else:
        response = 'Link CTL ACK %s' % details
    return _send_response(CorS, response, from_id, from_EN)

###########################################################################
# Gen-Z 1.0 "11.6 Link RFC"


def _Link_RFC(client, subelements, from_id, from_EN):
    if not client.SI.args.smart:
        client.SI.logmsg('I am not a manager')
        return False
    try:
        kv = CSV2dict(subelements[0])
        uS = int(kv['TTCuS'])
    except (IndexError, KeyError) as e:
        client.SI.logmsg('%d: Link RFC missing TTCuS' % client.id)
        return False
    except (TypeError, ValueError) as e:
        uS = 999999
    if uS > 1000:  # 1 ms, about the cycle time of this server
        client.SI.logmsg('Delay == %duS, dropping request' % uS)
        return False
    client.SID = client.SI.defaultSID
    client.CID = client.id * 100
    response = 'CtrlWrite Space=0,PFMSID=%d,PFMCID=%d,SID=%d,CID=%d' % (
        client.SI.defaultSID, client.SI.server_id * 100,
        client.SID, client.CID)
    return _send_response(client, response, from_id, from_EN)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL"


def _Link_CTL(client, subelements, from_id, from_EN):
    '''Subelements should be empty.'''
    if len(subelements) == 1 and subelements[0] == 'Peer-Attribute':
        details = 'C-Class=%s,SID0=%d,CID0=%d' % (
            client.SI.C_Class, client.SI.server_SID0, client.SI.server_CID0)
        nack = False
    else:
        details = 'Reason=Unsupported'
        nack = True
    return send_LinkACK(client, details, from_id, from_EN,nack=nack)

###########################################################################
# Finally a home


def _ping(client, subelements, from_id, from_EN):
    return _send_response(client, 'pong', from_id, from_EN)

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.
# Commands streams are case-sensitive, read the spec.


def switch_handler(client, request, from_id, from_EN):
    '''Return True if successfully parsed and processed.'''
    elements = request.split()
    try:
        handler, subelements = chelsea(elements)
        return handler(client, subelements, from_id, from_EN)
    except Exception as e:
        client.SI.logmsg(str(e))
    return False

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
