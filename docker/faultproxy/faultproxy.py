#!/usr/bin/env python3
"""
Fault-injection proxy for the valkey_fdw test suite.

Sits between PostgreSQL and Valkey and, on command, misbehaves in the exact
ways a real deployment eventually will: the connection drops mid-reply, the
server returns a malformed frame, a reply arrives after the client gave up.

Those paths are unreachable from a well-behaved server, which is how error
handling comes to be uniformly broken: nothing ever executes it, so nothing
ever reports that it does not work.

Control is over a separate TCP port, one line per command:

    arm <rule>       install a rule; it fires once and clears
    clear            remove any armed rule
    stats            report connection and byte counters
    ping             health check

Rules:

    drop_before <CMD>     close the connection before forwarding CMD upstream
    drop_after  <CMD>     forward CMD, then close before relaying the reply
    drop_mid    <CMD>     answer CMD with a frame that stops partway, then close
    garbage     <CMD>     answer CMD with a malformed RESP frame
    wrong_type  <CMD>     answer CMD with a well-formed reply of the wrong type
    truncate    <CMD>     answer CMD with a frame whose declared length lies
    delay <MS>  <CMD>     sleep MS before relaying CMD's reply
    huge        <CMD>     answer CMD with a multi-gigabyte declared bulk length
    no_resp3    <CMD>     answer CMD as a pre-6.0 server answers HELLO
    boolean     <CMD>     answer CMD with a RESP3 boolean
    empty_error <CMD>     answer CMD with an error carrying no message text
    noperm      <CMD>     answer CMD with an ACL denial for the keys it names

CMD is matched case-insensitively against the command verb, or '*' for any.
"""

import argparse
import re
import socket
import socketserver
import sys
import threading
import time

# --------------------------------------------------------------------------
# Armed rule, shared across connections. Guarded by a lock because the control
# port and the data path run on different threads.
# --------------------------------------------------------------------------
# Every action the data path implements. Armed rules are validated against
# this at arm time, and an unknown one is REFUSED rather than accepted.
#
# The counter alone cannot catch this. `arm` used to accept any word, the data
# path had no branch for it, the command was forwarded untouched - and
# take_rule had already counted it as fired. A suite asserting "a rule fired"
# therefore passed while nothing was injected, which is precisely the failure
# fault_fired() was added to prevent, one level further down.
#
# It is not hypothetical: it is how a proxy CONTAINER left running from an
# earlier topology, older than the image beside it, presents itself. The
# refusal turns that into a loud failure at arm time instead of a suite that
# quietly asserts nothing.
ACTIONS = frozenset((
    "drop_before", "drop_after", "drop_mid", "garbage", "wrong_type",
    "truncate", "delay", "huge", "bad_cursor", "bad_key", "bad_utf8",
    "no_resp3", "boolean", "empty_error", "noperm",
))

_lock = threading.Lock()
_rule = None            # dict(action=..., cmd=..., arg=...)
_stats = {"connections": 0, "c2s_bytes": 0, "s2c_bytes": 0, "faults_fired": 0}


def arm(action, cmd, arg=None):
    global _rule
    with _lock:
        _rule = {"action": action, "cmd": cmd.upper(), "arg": arg}


def clear():
    global _rule
    with _lock:
        _rule = None


def take_rule(verb):
    """Return and consume the armed rule if it matches this command verb."""
    global _rule
    with _lock:
        if _rule is None:
            return None
        if _rule["cmd"] != "*" and _rule["cmd"] != verb.upper():
            return None
        fired, _rule = _rule, None
        _stats["faults_fired"] += 1
        return fired


# --------------------------------------------------------------------------
# Minimal RESP command parsing.
#
# We only need the verb of each inbound command, and only enough framing to
# know where one command ends. Replies are relayed opaquely.
# --------------------------------------------------------------------------
_INLINE = re.compile(rb"^([A-Za-z_]+)")


def parse_verb(buf):
    """Best-effort verb extraction from a client command buffer."""
    if buf.startswith(b"*"):
        # Multibulk: *N\r\n$len\r\n<verb>\r\n...
        parts = buf.split(b"\r\n", 4)
        if len(parts) >= 3 and parts[1].startswith(b"$"):
            return parts[2].decode("latin-1", "replace")
        return ""
    m = _INLINE.match(buf)
    return m.group(1).decode("latin-1", "replace") if m else ""


GARBAGE = b"*2\r\n$5\r\nabc\r\n@@@notresp\r\n"
WRONG_TYPE = b":12345\r\n"
TRUNCATED = b"$100\r\nonly-a-few-bytes\r\n"
HUGE = b"$4294967295\r\n"

# Well-formed RESP whose TOP level is exactly the shape a SCAN reply has, and
# whose ELEMENTS are not. wrong_type above swaps the whole frame, so it can
# only exercise a top-level check; these two reach the code that walks into an
# array it has already accepted. A real server never sends them, which is the
# point: a client that reads element[0]->str without checking element[0]->type
# is relying on the far end, not on the library, and libvalkey parses
# faithfully whatever arrives.
BAD_CURSOR = b"*2\r\n:0\r\n*0\r\n"           # cursor is an integer
BAD_KEY = b"*2\r\n$1\r\n0\r\n*1\r\n:7\r\n"   # one key, and it is an integer

# An error reply whose text is not valid UTF-8. Valkey echoes the client's own
# arguments back inside some error messages, and this wrapper deliberately
# sends binary-safe keys, so a real server can produce one - but not on demand,
# which is why the proxy fabricates it. An ERROR is one of the few places a
# value reaches the client and the server log without passing through the read
# path's encoding check.
BAD_UTF8_ERROR = b"-ERR \xff\xfe not utf-8\r\n"

# What an ACL-restricted user is told when it reads a key it may not read.
# SCAN and the value fetch are separate commands with separate permissions, so
# a user allowed to enumerate the keyspace and refused the contents of part of
# it is an ordinary deployment rather than a broken one - and the refusal then
# arrives per key, in the middle of a page, where a scan is most tempted to
# treat it as a key that simply went away. A real server answers this for
# every matching key or none, so the mid-page shape is the proxy's to make.
NOPERM_ERROR = (b"-NOPERM this user has no permissions to access one of the "
                b"keys used as arguments\r\n")

# A frame that has begun and will never finish. Sent immediately before the
# connection is closed, so the client meets EOF partway through a reply it has
# already started parsing - which is a different failure from TRUNCATED, where
# the frame is short but the connection stays open and the reader waits.
PARTIAL = b"$100\r\nonly-a-few-bytes"

# What a server older than 6.0 says to HELLO. The wrapper asks for RESP3 on
# every cold connection and must tolerate a refusal by staying on RESP2 - and
# RESP2 is not a lesser version of the same shapes, it is DIFFERENT shapes:
# HGETALL comes back as a flat alternating array rather than a map, and
# ZRANGE ... WITHSCORES as alternating member/score rather than pairs. Reading
# one as the other yields the wrong row count with null members.
#
# Every image this suite runs against speaks RESP3, so without this the
# fallback halves of vfdw_row.c were dead code that would be found by a user
# on an old server rather than by us.
NO_RESP3 = b"-ERR unknown command 'HELLO'\r\n"

# A RESP3 boolean. Nothing in a stock Valkey produces one - Lua true becomes
# the integer 1 and false becomes nil under both protocols, which was checked
# against the running server rather than assumed - so the only way to reach
# the VALKEY_REPLY_BOOL arms is to fabricate one. libvalkey's reader parses
# RESP3 types off the wire regardless of what HELLO negotiated, so this
# arrives as a boolean even on a RESP2 connection.
BOOLEAN = b"#t\r\n"

# An error reply with no message text. A real server always sends some, so
# the "(no message)" branch existed for a case nobody could produce. It is not
# hypothetical defensive coding: pnstrdup(NULL, 0) is undefined behaviour, so
# the branch is what keeps a malformed error from being read as a string.
EMPTY_ERROR = b"-\r\n"


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        with _lock:
            _stats["connections"] += 1

        client = self.request
        try:
            upstream = socket.create_connection(
                (self.server.upstream_host, self.server.upstream_port), timeout=10)
        except OSError as exc:
            print(f"upstream connect failed: {exc}", file=sys.stderr, flush=True)
            client.close()
            return

        stop = threading.Event()

        def c2s():
            """Client -> server, inspecting each command for an armed rule."""
            try:
                while not stop.is_set():
                    data = client.recv(65536)
                    if not data:
                        break
                    with _lock:
                        _stats["c2s_bytes"] += len(data)

                    rule = take_rule(parse_verb(data))
                    if rule:
                        action = rule["action"]
                        if action == "drop_before":
                            stop.set()
                            break
                        if action == "garbage":
                            client.sendall(GARBAGE)
                            continue
                        if action == "wrong_type":
                            client.sendall(WRONG_TYPE)
                            continue
                        if action == "bad_cursor":
                            client.sendall(BAD_CURSOR)
                            continue
                        if action == "bad_key":
                            client.sendall(BAD_KEY)
                            continue
                        if action == "bad_utf8":
                            client.sendall(BAD_UTF8_ERROR)
                            continue
                        if action == "noperm":
                            client.sendall(NOPERM_ERROR)
                            continue
                        if action == "truncate":
                            client.sendall(TRUNCATED)
                            continue
                        if action == "huge":
                            client.sendall(HUGE)
                            continue
                        if action == "no_resp3":
                            # Deliberately NOT forwarded upstream: the real
                            # server must stay on RESP2 too, or it would send
                            # RESP3 shapes to a client that just concluded it
                            # is speaking RESP2 - a disagreement no real
                            # deployment can produce.
                            client.sendall(NO_RESP3)
                            continue
                        if action == "boolean":
                            client.sendall(BOOLEAN)
                            continue
                        if action == "empty_error":
                            client.sendall(EMPTY_ERROR)
                            continue
                        if action == "drop_mid":
                            client.sendall(PARTIAL)
                            try:
                                client.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                            stop.set()
                            break
                        if action == "delay":
                            time.sleep(int(rule["arg"]) / 1000.0)
                        upstream.sendall(data)
                        if action == "drop_after":
                            # Shut the client down here rather than signalling
                            # the reader thread: it is already blocked in
                            # recv() and would relay the reply before noticing
                            # a flag, making the fault a no-op.
                            try:
                                client.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                            stop.set()
                            break
                        continue

                    upstream.sendall(data)
            except OSError:
                pass
            finally:
                stop.set()

        def s2c():
            """Server -> client."""
            try:
                while not stop.is_set():
                    data = upstream.recv(65536)
                    if not data:
                        break
                    with _lock:
                        _stats["s2c_bytes"] += len(data)
                    client.sendall(data)
            except OSError:
                pass
            finally:
                stop.set()

        t1 = threading.Thread(target=c2s, daemon=True)
        t2 = threading.Thread(target=s2c, daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        for sock in (client, upstream):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


class ControlHandler(socketserver.StreamRequestHandler):
    def handle(self):
        for raw in self.rfile:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            parts = line.split()
            verb = parts[0].lower()

            if verb == "ping":
                self.wfile.write(b"+PONG\n")
            elif verb == "clear":
                clear()
                self.wfile.write(b"+OK\n")
            elif verb == "stats":
                with _lock:
                    snapshot = dict(_stats)
                body = " ".join(f"{k}={v}" for k, v in sorted(snapshot.items()))
                self.wfile.write(f"+{body}\n".encode())
            elif verb == "arm" and len(parts) >= 3:
                action = parts[1]
                if action not in ACTIONS:
                    self.wfile.write(
                        f"-ERR unknown action {action!r}; this proxy "
                        f"implements {' '.join(sorted(ACTIONS))}\n".encode())
                    self.wfile.flush()
                    continue
                if action == "delay" and len(parts) >= 4:
                    arm(action, parts[3], parts[2])
                else:
                    arm(action, parts[2])
                self.wfile.write(b"+OK\n")
            else:
                self.wfile.write(b"-ERR unrecognised control command\n")
            self.wfile.flush()


class ThreadedTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-port", type=int, default=6379)
    ap.add_argument("--control-port", type=int, default=6390)
    ap.add_argument("--upstream-host", default="valkey")
    ap.add_argument("--upstream-port", type=int, default=6379)
    args = ap.parse_args()

    data = ThreadedTCPServer(("0.0.0.0", args.listen_port), Handler)
    data.upstream_host = args.upstream_host
    data.upstream_port = args.upstream_port

    control = ThreadedTCPServer(("0.0.0.0", args.control_port), ControlHandler)

    threading.Thread(target=control.serve_forever, daemon=True).start()
    print(f"faultproxy: {args.listen_port} -> "
          f"{args.upstream_host}:{args.upstream_port}, "
          f"control on {args.control_port}", flush=True)
    data.serve_forever()


if __name__ == "__main__":
    main()
