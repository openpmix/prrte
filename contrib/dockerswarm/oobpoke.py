#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
"""Connect to a PRRTE daemon's OOB port as a process that is not a daemon.

  oobpoke.py ident  <uri> <version>   send the IDENT a daemon used to open with
  oobpoke.py hold   <uri> <count> <secs>  hold <count> idle connections open

<uri> is a file holding a daemon's OOB contact URI,
"<nspace>.<rank>;tcp://<ip>:<port>[:<mask>]" - the prte_hnp_uri value on every
prted's command line.

Used by run-tests.sh (test_rml) to show that knowing it no longer gets a
foreign connection treated as a daemon, and that connections left idle cannot
exhaust a daemon's listener.  Needs nothing but python3.
"""
import socket
import struct
import sys
import time


def parse(uri):
    ident, addr = uri.strip().split(";", 1)
    nspace = ident.rsplit(".", 1)[0]
    addr = addr.split(";")[0]
    assert addr.startswith("tcp://"), addr
    host, port = addr[len("tcp://"):].split(",")[0].split(":")[:2]
    return nspace, host, int(port)


def header(nspace, origin, dst, mtype, nbytes):
    ns = nspace.encode()
    # epoch, origin, dst, tag, seq_num, nbytes, type, nslen - network order
    return struct.pack("!QIIIIIBB", 1, origin, dst, 0, 0, nbytes, mtype, len(ns)) + ns


def ident(uri, version):
    """Send what opened a connection before authentication existed, and
    report what came back: REPLY_BYTES=<n>, and OPEN if the daemon kept the
    connection rather than closing it.  A daemon that took us for one of its
    own answers with its own IDENT and keeps the connection."""
    nspace, host, port = parse(uri)
    payload = struct.pack("!H", 1) + version.encode() + b"\0"
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(header(nspace, 7, 0, 1, len(payload)) + payload)
    s.settimeout(5)
    got = b""
    still_open = False
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            got += chunk
    except socket.timeout:
        still_open = True
    except OSError:
        pass
    print("REPLY_BYTES=%d" % len(got))
    if still_open:
        print("OPEN")
    return 0


def hold(uri, count, secs):
    _, host, port = parse(uri)
    socks = []
    for _ in range(count):
        try:
            socks.append(socket.create_connection((host, port), timeout=5))
        except OSError as e:
            print("connect failed after %d: %s" % (len(socks), e))
            break
    print("HELD=%d" % len(socks))
    sys.stdout.flush()
    time.sleep(secs)
    for s in socks:
        s.close()
    return 0


if __name__ == "__main__":
    if "ident" == sys.argv[1]:
        sys.exit(ident(open(sys.argv[2]).read(), sys.argv[3] if len(sys.argv) > 3 else "x"))
    if "hold" == sys.argv[1]:
        sys.exit(hold(open(sys.argv[2]).read(), int(sys.argv[3]), float(sys.argv[4])))
    sys.exit(1)
