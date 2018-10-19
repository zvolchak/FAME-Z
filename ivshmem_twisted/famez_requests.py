#!/usr/bin/python3

# Common routines for both server (switch) and clients which drill down
# on messages retrieved by parsing and generating a response.

import attr
import os
import functools
import sys

from collections import OrderedDict
from pprint import pprint

try:
    from famez_mailbox import FAMEZ_MailBox as MB
except ImportError as e:
    from .famez_mailbox import FAMEZ_MailBox as MB

PRINT = functools.partial(print, file=sys.stderr)
PPRINT = functools.partial(pprint, stream=sys.stderr)

###########################################################################
# Better than __slots__ although maybe this should be the precursor for
# making this entire file a class.  It's all items that cover the gamut
# of requests


RequestObject = attr.make_class('RequestObject',
    ['SID', 'CID', 'MB_slot', 'doorbell'])



###########################################################################
# Create a subroutine name out of the elements passed in.  Return
# the remainder.  Start with the least-specific construct.


def _unprocessed(client, *args, **kwargs):
    return False
    cmdlineargs = getattr(client, 'args', client.SI.args)
    if client.SI.args.verbose:
        _logmsg('NOOP', args, kwargs)
    return False


def chelsea(elements, verbose=0):
    entry = ''          # They begin with a leading '_', wait for it...
    G = globals()
    for i, e in enumerate(elements):
        e = e.replace('-', '_')         # Such as 'Link CTL Peer-Attribute'
        entry += '_%s' % e
        if verbose > 1:
            print('Looking for %s()...' % entry, end='', file=sys.stderr)
        if entry in G:
            args = elements[i + 1:]
            if verbose > 1:
                print('found it->%s' % str(args), file=sys.stderr)
            return G[entry], args
        if verbose > 1:
            print('NOPE', file=sys.stderr)
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
# Here instead of famez_mailbox to manage the tag.  Can be called as a
# "discussion initiator" usually from the REPL interpreters, or as a
# response to a received command from the callbacks.

_next_tag = 1               # Gen-Z tag field

_tagged = OrderedDict()     # By tag, just store receiver now

_tracker = 0                # FAME-Z addenda to watch conversations

_TRACKER_TOKEN = '!FZT='

def send_payload(payload, from_id, to_doorbell, tag=None, reset_tracker=False):
    global _next_tag, _tracker

    # sender_id = receiver.responder_id
    # doorbell = receiver.responder_EN
    if tag is not None:     # zero-length string can trigger this
        payload += ',Tag=%d' % _next_tag
        _tagged[str(_next_tag)] = '%d.%d!%s|%s' % (
            receiver.SID0, receiver.CID0, payload, tag)
        _next_tag += 1

    # Put the tracker on the end where it's easier to find
    if reset_tracker:
        _tracker = 0
    _tracker += 1
    payload += '%s%d' % (_TRACKER_TOKEN, _tracker)

    ret = MB.fill(from_id, payload)     # True == no timeout, no stomp
    to_doorbell.ring()
    return ret

###########################################################################
# Gen-Z 1.0 "6.8 Standalone Acknowledgment"
# Received by server/switch


def _Standalone_Acknowledgment(response_receiver, args):
    retval = True
    tag = False
    try:
        kv = CSV2dict(args[0])
        stamp, tag = _tagged[kv['Tag']].split('|')
        del _tagged[kv['Tag']]
        tag = tag.strip()
        kv = CSV2dict(tag)
    except KeyError as e:
        _tracer('UNTAGGING %d:%s FAILED' %
            (response_receiver.responder_id, response_receiver.nodename))
        retval = False
        kv = {}

    afterACK = kv.get('AfterACK', False)
    if afterACK:
        send_payload(after_ACK,
                     response_receiver.responder_id,
                     response_receiver.responder_EN)

    if _tagged:
        print('Outstanding tags:', file=sys.stderr)
        pprint(_tagged, stream=sys.stderr)
    return 'dump'


def _send_SA(response_receiver, tag, reason):
    payload = 'Standalone Acknowledgment Tag=%s,Reason=%s' % (tag, reason)
    return send_payload(payload,
                        response_receiver.responder_id,
                        response_receiver.responder_EN)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL" subfield
# Sent by clients


def send_LinkACK(response_receiver, details, nack=False):
    if nack:
        payload = 'Link CTL NAK %s' % details
    else:
        payload = 'Link CTL ACK %s' % details
    return send_payload(payload,
                        response_receiver.responder_id,
                        response_receiver.responder_EN)

###########################################################################
# Gen-Z 1.0 "6.10.1 P2P Core..."
# Received by client, only really expecting RFC data


def _CTL_Write(response_receiver, args):
    kv = CSV2dict(args[0])
    if int(kv['Space']) != 0:
        return False
    response_receiver.SID0 = int(kv['SID'])
    response_receiver.CID0 = int(kv['CID'])
    response_receiver.linkattrs['State'] = 'configured'
    return _send_SA(response_receiver, kv['Tag'], 'OK')

###########################################################################
# Gen-Z 1.0 "11.6 Link RFC"
# Received by switch


def _Link_RFC(response_receiver, args):
    if not response_receiver.SI.args.smart:
        _logmsg('I am not a manager')
        return False
    try:
        kv = CSV2dict(args[0])
        delay = kv['TTC'].lower()
    except (IndexError, KeyError) as e:
        _logmsg('%d: Link RFC missing TTC' % response_receiver.id)
        return False
    if not 'us' in delay:  # greater than cycle time of this server
        _logmsg('Delay %s is too long, dropping request' % delay)
        return False
    payload = 'CTL-Write Space=0,PFMSID=%d,PFMCID=%d,SID=%d,CID=%d' % (
        response_receiver.SI.server_SID0, response_receiver.SI.server_CID0,
        response_receiver.SID0, response_receiver.CID0)
    return send_payload(payload,
                        response_receiver.responder_id,
                        response_receiver.responder_EN,
                        tag='AfterACK=Link CTL Peer-Attribute')

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL"
# Entered on both client and server responses.


def _Link_CTL(response_receiver, args):
    '''Subelements should be empty.'''
    arg0 = args[0] if len(args) else ''
    if len(args) == 1:
        if arg0 == 'Peer-Attribute':
            if getattr(response_receiver.SI, 'isPFM', None) is None:
                SID0 = response_receiver.SID0
                CID0 = response_receiver.CID0
                cclass = response_receiver.cclass
            else:
                SID0 = response_receiver.SI.server_SID0
                CID0 = response_receiver.SI.server_CID0
                cclass = MB.cclass(MB.server_id)
            attrs = 'C-Class=%s,CID0=%d,SID0=%d' % (cclass, CID0, SID0)
                # MB.cclass(response_receiver.id), CID0, SID0)
            return send_LinkACK(response_receiver, attrs)

    if arg0 == 'ACK' and len(args) == 2:
        # FIXME: correlation ala _tagged?  How do I know it's peer attrs?
        # FIXME: add a key to the response...
        response_receiver.peerattrs = CSV2dict(args[1])
        return 'dump'

    if arg0 == 'NAK':
        # FIXME: do I track the sender ala _tagged and deal with it?
        print('Got a NAK, not sure what to do with it.', file=sys.stderr)
        return False

    _logmsg( 'Got %s from %d' % (str(args), response_receiver.id))
    return False

###########################################################################
# Finally a home


def _ping(response_receiver, args):
    return send_payload('pong',
                        response_receiver.responder_id,
                        response_receiver.responder_EN)


def _dump(response_receiver, args):
    return 'dump'      # Technically "True", but with semantics


###########################################################################
# Chained from actual EventReader callback in twisted_server.py.
# Commands streams are case-sensitive, read the spec.
# Return True if successfully parsed and processed.

_logmsg = None
_tracer = None


def handle_request(request, requester_name, response_receiver):
    global _logmsg, _tracer

    if _logmsg is None:
        _logmsg = response_receiver.SI.logmsg   # FIXME: logger.logger...
        _tracer = response_receiver.SI.trace

    elements = request.split(_TRACKER_TOKEN)
    payload = elements.pop(0)
    trace = '\n%10s@%d->"%s"' % (
        requester_name, response_receiver.requester_id, payload)
    FTZ = int(elements[0]) if elements else False
    if FTZ:
        trace += ' (%d)' % FTZ
        _tracker = FTZ
    _tracer(trace)

    elements = payload.split()
    try:
        handler, args = chelsea(elements, response_receiver.SI.args.verbose)
        return handler(response_receiver, args)
    except KeyError as e:
        _logmsg('KeyError: %s' % str(e))
    except Exception as e:
        _logmsg(str(e))
    return False
