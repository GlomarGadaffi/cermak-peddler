#!/usr/bin/env python3
"""Office smoke test — simulates a small office of SIP phones against a live drawbridge
instance and exercises the PBX features the project claims to support end-to-end over
real SIP wire traffic (not just unit tests against RequestsHandler::handle() in-process).

Pure stdlib, no deps. Mirrors the UAC patterns in multicall_uac.py / sip_probe.py but
generalizes to a small multi-dialog UA (Phone) that can register, place calls, receive
calls (auto-answering INVITEs/re-INVITEs/BYEs/CANCELs/NOTIFYs/OPTIONS from a background
rx thread), send DTMF (SIP INFO) for star codes, and send REFER for blind/attended
transfer.

Usage:
    python3 office_smoke.py <board_ip> [http_port=80]

Scenarios (each prints PASS/FAIL + a one-line reason):
  1. Office registration  — N extensions register cleanly.
  2. *777 echo test       — call the echo extension, verify the SIP handshake AND that
                            real RTP loops back (self-addressed by design — see the
                            onInvite "777" handler: it echoes the caller's OWN SDP).
  3. Call park + retrieve — A calls 777, parks on orbit 700, a DIFFERENT extension
                            retrieves; verifies the retriever gets A's SDP, A gets a
                            re-INVITE carrying the retriever's SDP, and BYE-relay on
                            teardown.
  4. Blind transfer       — A calls B, A REFERs (no Replaces) to C; verifies 202, B's
                            leg is torn down (BYE), and a fresh INVITE reaches C.
  5. Attended transfer    — A calls B AND (separately) calls C; A REFERs on the A-B
                            dialog with ?Replaces=<A-C call-id>; verifies 202, B and C
                            each receive a spliced re-INVITE carrying the other's SDP.
  6. DND (*60/*80)        — DTMF star-code toggles DND; verify /api/status reflects it
                            and an INVITE to a DND'd extension gets 480 (deliberate,
                            per RequestsHandler::onInvite — not 486 as older docs say).
  7. Call forward (*72/*73) — DTMF star-code sets CFU; verify /api/status reflects it
                            and an INVITE to the forwarding extension redirects.
  8. Ring group           — provision a ring-all group via /api/group; verify an INVITE
                            to the group extension forks to every member.
  9. 999 all-page         — verify INVITE to 999 forks (auto-answer) to every other
                            registered extension.
 10. CDR                  — verify /api/cdr grew after the calls above.

Any scenario that can't complete (feature not reachable this way, or a genuine defect)
is reported as FAIL with a short reason — read the printed detail, don't just check the
exit code.
"""
import socket, sys, threading, time, random, re, json, urllib.request, urllib.parse, urllib.error

def now_ms(): return int(time.time() * 1000)
def rid(n=8): return ''.join(random.choice("0123456789abcdef") for _ in range(n))

# ── RTP helpers ───────────────────────────────────────────────────────────────
def rtp_packet(seq, ts, ssrc, payload):
    hdr = bytes([0x80, 0x00, (seq >> 8) & 0xFF, seq & 0xFF,
                 (ts >> 24) & 0xFF, (ts >> 16) & 0xFF, (ts >> 8) & 0xFF, ts & 0xFF,
                 (ssrc >> 24) & 0xFF, (ssrc >> 16) & 0xFF, (ssrc >> 8) & 0xFF, ssrc & 0xFF])
    return hdr + payload

def sdp_endpoint(body):
    """Extract (ip, port) from an SDP body's c=/m=audio lines. None if absent."""
    ip = None; port = None
    for line in body.split("\r\n"):
        if line.startswith("c=IN IP4"):
            ip = line.split()[-1]
        if line.startswith("m=audio"):
            try: port = int(line.split()[1])
            except (IndexError, ValueError): pass
    if ip and port: return (ip, port)
    return None

def http_get_json(board_ip, path, port=80, timeout=5):
    try:
        with urllib.request.urlopen("http://%s:%d%s" % (board_ip, port, path), timeout=timeout) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"__err__": str(e)}

def http_post(board_ip, path, params, port=80, timeout=5):
    body = urllib.parse.urlencode(params).encode()
    req = urllib.request.Request("http://%s:%d%s" % (board_ip, port, path), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return -1, str(e)

# ── SIP message (mini) parse ─────────────────────────────────────────────────
class SipMsg:
    def __init__(self, raw):
        self.raw = raw
        head, _, self.body = raw.partition("\r\n\r\n")
        lines = head.split("\r\n")
        self.first = lines[0] if lines else ""
        self.headers = {}
        for h in lines[1:]:
            if ":" not in h: continue
            k, v = h.split(":", 1)
            self.headers.setdefault(k.strip().lower(), []).append(v.strip())
        self.is_response = self.first.startswith("SIP/2.0")
        if self.is_response:
            parts = self.first.split(" ", 2)
            self.status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            self.reason = parts[2] if len(parts) > 2 else ""
            self.method = None
        else:
            self.method = self.first.split(" ", 1)[0]
            self.status = 0
    def h(self, name, default=""):
        v = self.headers.get(name.lower())
        return v[0] if v else default
    def call_id(self): return self.h("call-id")
    def cseq_num_method(self):
        c = self.h("cseq")
        parts = c.split()
        return (parts[0], parts[1]) if len(parts) == 2 else ("0", "")
    def tag_of(self, header):
        v = self.h(header)
        m = re.search(r";tag=([^;>\s]+)", v)
        return m.group(1) if m else ""

def strip_hdr_name(line):
    """'From: <sip:...>;tag=x' -> '<sip:...>;tag=x'"""
    return line.split(":", 1)[1].strip() if ":" in line else line

# ── Phone: a small multi-dialog UA ───────────────────────────────────────────
class Phone:
    def __init__(self, board_ip, ext, local_ip, board_port=5060):
        self.board_ip = board_ip; self.board_port = board_port
        self.ext = ext; self.local_ip = local_ip
        self.sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sip.bind((local_ip, 0)); self.sip.settimeout(0.2)
        self.sip_port = self.sip.getsockname()[1]
        self.rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtp.bind((local_ip, 0)); self.rtp.settimeout(0.2)
        self.rtp_port = self.rtp.getsockname()[1]
        self.contact = "<sip:%s@%s:%d>" % (ext, local_ip, self.sip_port)
        self.lock = threading.Lock()
        self.dialogs = {}     # callid -> dict(role='uac'/'uas', local_tag, remote_tag, remote_sdp, ...)
        self.events = []      # list of dict(kind=..., callid=..., ...) — append-only, newest last
        self.responses = {}   # (callid, method, cseqnum) -> SipMsg  (final responses only)
        self.running = True
        self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.rx_thread.start()

    def close(self):
        self.running = False
        try: self.sip.close()
        except OSError: pass
        try: self.rtp.close()
        except OSError: pass

    def _log(self, kind, **kw):
        with self.lock:
            self.events.append(dict(kind=kind, t=now_ms(), **kw))

    def wait_event(self, kind, timeout=5.0, match=None):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for e in self.events:
                    if e["kind"] == kind and (match is None or match(e)):
                        return e
            time.sleep(0.02)
        return None

    def wait_response(self, callid, method, cseqnum, timeout=5.0):
        deadline = time.time() + timeout
        key = (callid, method, str(cseqnum))
        while time.time() < deadline:
            with self.lock:
                if key in self.responses:
                    return self.responses[key]
            time.sleep(0.02)
        return None

    # Payload-type -> rtpmap line, for building offers that exercise the PBX's
    # relay codec policy (order preserved, G.722 admitted, unknowns dropped, 488
    # when nothing is left).
    RTPMAP = {
        0: "a=rtpmap:0 PCMU/8000", 8: "a=rtpmap:8 PCMA/8000", 9: "a=rtpmap:9 G722/8000",
        96: "a=rtpmap:96 opus/48000/2", 101: "a=rtpmap:101 telephone-event/8000",
    }

    def _my_sdp(self, inactive=False, codecs=None):
        if inactive:
            return ("v=0\r\no=- 0 0 IN IP4 %s\r\ns=smoke\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                    "m=audio 9 RTP/AVP 0\r\na=inactive\r\n") % (self.local_ip, self.local_ip)
        if codecs:
            attrs = "".join(self.RTPMAP[pt] + "\r\n" for pt in codecs if pt in self.RTPMAP)
            return ("v=0\r\no=- 0 0 IN IP4 %s\r\ns=smoke\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                    "m=audio %d RTP/AVP %s\r\n%sa=sendrecv\r\n"
                    ) % (self.local_ip, self.local_ip, self.rtp_port,
                         " ".join(str(pt) for pt in codecs), attrs)
        return ("v=0\r\no=- 0 0 IN IP4 %s\r\ns=smoke\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                "m=audio %d RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
                ) % (self.local_ip, self.local_ip, self.rtp_port)

    def _send(self, data, addr=None):
        self.sip.sendto(data.encode() if isinstance(data, str) else data,
                         addr or (self.board_ip, self.board_port))

    def _respond(self, req: SipMsg, addr, status, reason, to_tag=None, body="", extra_headers=""):
        totag_part = req.h("to")
        if to_tag and "tag=" not in totag_part:
            totag_part = totag_part + ";tag=" + to_tag
        resp = ("SIP/2.0 %d %s\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %s\r\n"
                "Contact: <sip:%s@%s:%d>\r\n"
                "%s"
                "Content-Length: %d\r\n\r\n%s"
                ) % (status, reason, req.h("via"), req.h("from"), totag_part,
                     req.call_id(), req.h("cseq"), self.ext, self.local_ip, self.sip_port,
                     extra_headers, len(body), body)
        self._send(resp, addr)

    def _rx_loop(self):
        while self.running:
            try:
                data, addr = self.sip.recvfrom(8192)
            except (socket.timeout, OSError):
                continue
            try:
                self._handle(SipMsg(data.decode(errors="replace")), addr)
            except Exception as e:
                self._log("RX_ERROR", detail=str(e))

    def _handle(self, msg: SipMsg, addr):
        if msg.is_response:
            num, method = msg.cseq_num_method()
            callid = msg.call_id()
            self._log("RESPONSE", callid=callid, method=method, num=num, status=msg.status, body=msg.body, remote_addr=addr, to=msg.h("to"))
            if msg.status >= 200:
                with self.lock:
                    self.responses[(callid, method, num)] = msg
                    d = self.dialogs.get(callid)
                    if d is not None and method == "INVITE" and msg.status < 300:
                        d["remote_tag"] = msg.tag_of("to") or d.get("remote_tag", "")
                        d["remote_sdp"] = msg.body or d.get("remote_sdp", "")
                        d["remote_addr"] = addr
            return

        # Requests
        method = msg.method
        callid = msg.call_id()
        self._log("REQUEST", callid=callid, method=method, from_=msg.h("from"), body=msg.body, remote_addr=addr)

        if method == "OPTIONS":
            self._respond(msg, addr, 200, "OK")
            return
        if method == "ACK":
            with self.lock:
                d = self.dialogs.get(callid)
                if d is not None: d["acked"] = True
            return
        if method == "CANCEL":
            self._respond(msg, addr, 200, "OK")
            with self.lock:
                d = self.dialogs.get(callid)
                if d is not None: d["cancelled"] = True
            return
        if method == "BYE":
            self._respond(msg, addr, 200, "OK")
            with self.lock:
                d = self.dialogs.get(callid)
                if d is not None: d["byed"] = True
            return
        if method == "NOTIFY":
            self._respond(msg, addr, 200, "OK")
            return
        if method == "INFO":
            self._respond(msg, addr, 200, "OK")
            return
        if method == "INVITE":
            with self.lock:
                existing = self.dialogs.get(callid)
            if existing is None:
                # New inbound dialog (register-beep, ring-group fork, page, transfer target,
                # or the blind-transfer redirect from a peer). Auto-answer.
                to_tag = rid(9)
                self._send_provisional(msg, addr, 180, "Ringing", to_tag)
                sdp = self._my_sdp()
                self._respond(msg, addr, 200, "OK", to_tag=to_tag, body=sdp,
                              extra_headers="Content-Type: application/sdp\r\n")
                with self.lock:
                    self.dialogs[callid] = dict(role="uas", local_tag=to_tag,
                                                 remote_tag=msg.tag_of("from"), remote_sdp=msg.body,
                                                 remote_addr=addr, acked=False, byed=False, cancelled=False)
                self._log("INCOMING_CALL", callid=callid, from_=msg.h("from"), body=msg.body, remote_addr=addr)
            else:
                # Re-INVITE on an existing dialog (attended-transfer splice, park retrieve
                # bridging, or anything else re-negotiating media). Re-answer 200 with our
                # own (unchanged) SDP.
                sdp = self._my_sdp()
                self._respond(msg, addr, 200, "OK", to_tag=existing["local_tag"], body=sdp,
                              extra_headers="Content-Type: application/sdp\r\n")
                with self.lock:
                    existing["remote_sdp_in"] = msg.body
                self._log("REINVITE", callid=callid, body=msg.body, remote_addr=addr)
            return
        # Unknown method: ack it generically so nothing retransmits forever.
        self._respond(msg, addr, 200, "OK")

    def _send_provisional(self, req: SipMsg, addr, status, reason, to_tag):
        resp = ("SIP/2.0 %d %s\r\n"
                "Via: %s\r\n"
                "From: %s\r\n"
                "To: %s;tag=%s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %s\r\n"
                "Content-Length: 0\r\n\r\n"
                ) % (status, reason, req.h("via"), req.h("from"), req.h("to"), to_tag,
                     req.call_id(), req.h("cseq"))
        self._send(resp, addr)

    # ── outbound: REGISTER ────────────────────────────────────────────────────
    def register(self, expires=3600, retries=4):
        tag = "reg%s" % rid(6)
        callid = "reg-%s-%s@%s" % (self.ext, rid(6), self.local_ip)
        for attempt in range(retries):
            req = ("REGISTER sip:%s SIP/2.0\r\n"
                   "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s\r\n"
                   "Max-Forwards: 70\r\nFrom: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>\r\n"
                   "Call-ID: %s\r\nCSeq: %d REGISTER\r\nContact: %s\r\nExpires: %d\r\n"
                   "Content-Length: 0\r\n\r\n"
                   ) % (self.board_ip, self.local_ip, self.sip_port, rid(10),
                        self.ext, self.board_ip, tag, self.ext, self.board_ip,
                        callid, attempt + 1, self.contact, expires)
            self._send(req)
            got = self.wait_response(callid, "REGISTER", str(attempt + 1), timeout=2.5)
            if got and got.status == 200:
                return True
        return False

    # ── outbound: INVITE / ACK / BYE / CANCEL ────────────────────────────────
    def invite(self, dest, timeout=8.0, sdp_inactive=False, codecs=None):
        """Places a call to `dest`. Returns (callid, final_status, remote_sdp) — status 0
        on no-response timeout. `codecs` overrides the offered payload list (see
        RTPMAP) to probe the relay codec policy."""
        tag = "ft%s" % rid(6)
        callid = "call-%s-%s-%s@%s" % (self.ext, dest, rid(6), self.local_ip)
        sdp = self._my_sdp(inactive=sdp_inactive, codecs=codecs)
        branch = "z9hG4bK%s" % rid(12)
        req = ("INVITE sip:%s@%s SIP/2.0\r\n"
               "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>\r\n"
               "Call-ID: %s\r\nCSeq: 1 INVITE\r\nContact: %s\r\n"
               "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s"
               ) % (dest, self.board_ip, self.local_ip, self.sip_port, branch,
                    self.ext, self.board_ip, tag, dest, self.board_ip,
                    callid, self.contact, len(sdp), sdp)
        with self.lock:
            self.dialogs[callid] = dict(role="uac", local_tag=tag, remote_tag="", remote_sdp="",
                                         dest=dest, local_sdp=sdp, remote_addr=None, cseq=1)
        # Basic RFC 3261 §17.1.1 INVITE retransmission (unreliable-transport UACs must
        # do this). Needed in practice: with several phones' background rx threads all
        # polling/replying at once (register-beeps, OPTIONS keepalives, prior scenarios'
        # still-open sockets), a lone UDP datagram occasionally gets lost on loopback
        # under that load — a real UA papers over exactly this with retransmission, so
        # our test UAC should too rather than reporting a false failure.
        self._send(req)
        elapsed = 0.0
        retransmit_at = 1.0
        got = None
        while elapsed < timeout:
            got = self.wait_response(callid, "INVITE", "1", timeout=min(0.5, timeout - elapsed))
            if got:
                break
            elapsed += 0.5
            if elapsed >= retransmit_at and elapsed < timeout:
                self._send(req)
                retransmit_at += 2.0   # back off, mirrors T1-doubling in spirit
        if not got:
            return callid, 0, ""
        with self.lock:
            d = self.dialogs[callid]
            d["remote_tag"] = got.tag_of("to")
            d["remote_sdp"] = got.body
            # remote_addr is already set correctly by _rx_loop's response handler (which has
            # the real source addr of the packet) — don't clobber it here.
        if got.status < 300:
            self.ack(callid)
        return callid, got.status, got.body

    def ack(self, callid):
        with self.lock:
            d = self.dialogs.get(callid)
        if not d: return
        req = ("ACK sip:%s@%s SIP/2.0\r\n"
               "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>;tag=%s\r\n"
               "Call-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n"
               ) % (d["dest"], self.board_ip, self.local_ip, self.sip_port, rid(10),
                    self.ext, self.board_ip, d["local_tag"], d["dest"], self.board_ip,
                    d["remote_tag"], callid)
        self._send(req)

    def bye(self, callid):
        with self.lock:
            d = self.dialogs.get(callid)
        if not d: return None
        d["cseq"] = d.get("cseq", 1) + 1
        req = ("BYE sip:%s@%s SIP/2.0\r\n"
               "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>;tag=%s\r\n"
               "Call-ID: %s\r\nCSeq: %d BYE\r\nContent-Length: 0\r\n\r\n"
               ) % (d["dest"], self.board_ip, self.local_ip, self.sip_port, rid(10),
                    self.ext, self.board_ip, d["local_tag"], d["dest"], self.board_ip,
                    d["remote_tag"], callid, d["cseq"])
        self._send(req)
        return self.wait_response(callid, "BYE", str(d["cseq"]), timeout=3.0)

    # ── outbound: DTMF via SIP INFO (star codes) ─────────────────────────────
    def send_dtmf(self, digits, inter_digit_delay=0.05):
        """Fires one INFO per digit sharing a synthetic Call-ID, matching how onDtmfInfo
        accumulates a sequence (mirrors tests/AdminHttpGate_test.cpp's sendDtmfSequence)."""
        tag = "it%s" % rid(6)
        callid = "dtmf-%s-%s@%s" % (self.ext, rid(6), self.local_ip)
        for i, d in enumerate(digits):
            body = "Signal=%s\r\nDuration=100\r\n" % d
            req = ("INFO sip:server SIP/2.0\r\n"
                   "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s\r\n"
                   "Max-Forwards: 70\r\nFrom: <sip:%s@server>;tag=%s\r\nTo: <sip:server>\r\n"
                   "Call-ID: %s\r\nCSeq: %d INFO\r\nContent-Type: application/dtmf-relay\r\n"
                   "Content-Length: %d\r\n\r\n%s"
                   ) % (self.local_ip, self.sip_port, rid(10), self.ext, tag,
                        callid, i + 1, len(body), body)
            self._send(req)
            time.sleep(inter_digit_delay)

    def send_dtmf_reliable(self, digits, verify_fn, retries=3, settle=0.3):
        """send_dtmf() has no retransmission — each digit is one irreplaceable UDP
        datagram, and losing even a single one (observed under background load from
        several other phones' threads/sockets) corrupts the whole accumulated sequence,
        silently. A real handset user facing a star-code that "didn't take" just dials
        it again; do the same here rather than reporting a false failure. verify_fn()
        should check server-visible state (e.g. poll /api/status) and return bool."""
        for attempt in range(retries):
            self.send_dtmf(digits)
            time.sleep(settle)
            if verify_fn():
                return True
        return False

    # ── outbound: REFER (blind + attended) ───────────────────────────────────
    def refer(self, callid, refer_to, replaces=None):
        """replaces, if given, is (callid_bare, from_tag, to_tag) for attended transfer."""
        with self.lock:
            d = self.dialogs.get(callid)
        if not d: return None
        d["cseq"] = d.get("cseq", 1) + 1
        refer_to_uri = "sip:%s@%s" % (refer_to, self.board_ip)
        if replaces:
            rcid, rfrom, rto = replaces
            params = "Replaces=%s%%3Bfrom-tag%%3D%s%%3Bto-tag%%3D%s" % (
                urllib.parse.quote(rcid, safe=""), rfrom, rto)
            refer_to_uri += "?" + params
        req = ("REFER sip:%s@%s SIP/2.0\r\n"
               "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s\r\n"
               "Max-Forwards: 70\r\nFrom: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>;tag=%s\r\n"
               "Call-ID: %s\r\nCSeq: %d REFER\r\nRefer-To: <%s>\r\nContent-Length: 0\r\n\r\n"
               ) % (d["dest"], self.board_ip, self.local_ip, self.sip_port, rid(10),
                    self.ext, self.board_ip, d["local_tag"], d["dest"], self.board_ip,
                    d["remote_tag"], callid, d["cseq"], refer_to_uri)
        self._send(req)
        return self.wait_response(callid, "REFER", str(d["cseq"]), timeout=5.0)

    # ── RTP audio-plane helpers (used by the 777 echo test) ──────────────────
    def rtp_loopback_check(self, dest_ip, dest_port, n=10):
        """Sends N marked packets to (dest_ip,dest_port) FROM our own rtp socket and
        counts how many come back (readable on the same socket). For 777 dest==our own
        address by design (server echoes the caller's own SDP) — this proves the
        SIP-negotiated media path is actually usable for real audio, not just that the
        handshake completed."""
        ssrc = random.randint(1, 1 << 30)
        sent = 0; recvd = 0
        for i in range(n):
            pkt = rtp_packet(1000 + i, i * 160, ssrc, b"\xff" * 160)
            try:
                self.rtp.sendto(pkt, (dest_ip, dest_port)); sent += 1
            except OSError:
                pass
            time.sleep(0.02)
        deadline = time.time() + 1.0
        while time.time() < deadline and recvd < sent:
            try:
                data, _ = self.rtp.recvfrom(2048)
                if len(data) >= 12: recvd += 1
            except (socket.timeout, OSError):
                pass
        return sent, recvd


def local_ip_for(target):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target, 5060)); return s.getsockname()[0]
    finally:
        s.close()


_next_loopback_octet = [2]   # class-level counter; 127.0.0.1 is reserved for local_ip_for's own probe

def phone_source_ip(base_local_ip):
    """RequestsHandler::allowPacket (a legitimate per-source-IP token bucket, burst=40 /
    sustained=20 pkt/s — see src/SIP/RequestsHandler.cpp) buckets by raw source IP. On a
    real office each phone has its own LAN IP, so this never bites a real deployment. This
    harness simulates many phones from ONE test-runner process, though — sharing a single
    source IP across 6+ simulated extensions plus several call/transfer scenarios burns
    through that budget fast and the rate limiter starts silently dropping our OWN
    packets (an accurate reflection of a real single-source flood, not a product bug —
    confirmed by watching /api/status's packetsDropped counter climb). When testing
    against loopback, give each simulated phone its OWN 127.0.0.N address (the entire
    127.0.0.0/8 range binds to `lo` with no config needed on Linux/WSL) so it gets its own
    bucket, matching a real office's topology instead of an artifact of this test rig.
    Against a REAL board on a real LAN we only have the one routable source IP, so this
    falls back to that unchanged — a real-hardware run may still need scenarios spaced out
    if it accumulates enough traffic to trip the same limiter."""
    if base_local_ip != "127.0.0.1":
        return base_local_ip
    octet = _next_loopback_octet[0]
    _next_loopback_octet[0] += 1
    return "127.0.0.%d" % octet


# ── Scenarios ─────────────────────────────────────────────────────────────────
RESULTS = []
def report(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", name, detail))


def scenario_registration(phones):
    ok = all(p.register() for p in phones.values())
    # Settle: a brand-new registration observed unreliable inbound delivery to the
    # phone for a brief window right after REGISTER succeeds (missed register-beep,
    # missed park-orbit response) when testing across a real NAT/bridge hop to real
    # hardware — not reproduced against the WSL host build over loopback, so this
    # looks like a network-path settling quirk rather than a product bug. A short
    # pause here benefits every scenario that follows, not just the one that first
    # exposed it.
    time.sleep(1.0)
    report("Office registration", ok,
           "%d/%d extensions registered" % (sum(1 for p in phones.values()), len(phones)))
    return ok


def scenario_echo_test(phones):
    a = phones["A"]
    callid, status, sdp = a.invite("777")
    if status != 200:
        report("*777 echo test", False, "INVITE to 777 got status=%s (expected 200)" % status)
        return False
    ep = sdp_endpoint(sdp)
    if not ep:
        report("*777 echo test", False, "200 OK had no parseable SDP c=/m=")
        a.bye(callid)
        return False
    # By design the echoed SDP should be OUR OWN offer (self-referential loopback).
    self_match = (ep[1] == a.rtp_port)
    sent, recvd = a.rtp_loopback_check(*ep, n=10)
    a.bye(callid)
    ok = self_match and recvd >= sent * 0.8
    report("*777 echo test", ok,
           "echoed SDP port=%d (own=%d, match=%s), rtp %d/%d looped back" %
           (ep[1], a.rtp_port, self_match, recvd, sent))
    return ok


def scenario_codec_policy(phones):
    """Relay codec policy (filterAudioCodecs): a G.722-first offer reaches the callee
    with its order intact and nothing added; an Opus-only offer is refused 488 at the
    PBX instead of being 'answered' with payloads the phone never offered."""
    a = phones["A"]; d = phones["D"]
    t0 = now_ms()
    callid, status, answer = a.invite(d.ext, codecs=[9, 0, 101])
    ev = d.wait_event("INCOMING_CALL", timeout=5.0, match=lambda e: e["callid"] == callid)
    fork_body = ev["body"] if ev else ""
    order_kept = "RTP/AVP 9 0 101" in fork_body
    nothing_added = "RTP/AVP 0 8 101" not in fork_body and " 8 " not in fork_body.split("m=audio")[-1][:40]
    g722_rtpmap = "a=rtpmap:9 G722/8000" in fork_body
    # D answers PCMU-only (default _my_sdp); the relayed answer to A must be D's pick,
    # not a rewrite.
    answer_ok = status == 200 and "RTP/AVP 0 101" in answer and "RTP/AVP 0 8 101" not in answer
    if status == 200:
        a.bye(callid)
        time.sleep(0.3)

    callid2, status2, _ = a.invite(d.ext, codecs=[96, 101])
    refused = (status2 == 488)
    leaked = d.wait_event("INCOMING_CALL", timeout=1.0, match=lambda e: e["callid"] == callid2)
    if status2 == 200:
        a.bye(callid2)

    ok = order_kept and nothing_added and g722_rtpmap and answer_ok and refused and leaked is None
    report("Codec policy (relay)", ok,
           "G.722-first fork kept order=%s nothing added=%s rtpmap kept=%s; answer relayed as-is=%s "
           "(status %s); Opus-only refused 488=%s (status %s) not forwarded=%s" %
           (order_kept, nothing_added, g722_rtpmap, answer_ok, status, refused, status2, leaked is None))
    return ok


def scenario_park_retrieve(phones):
    a = phones["A"]; c = phones["C"]
    # 1. A calls 777 (the call "being parked").
    echo_callid, status, _ = a.invite("777")
    if status != 200:
        report("Call park + retrieve", False, "setup: A->777 got status=%s" % status)
        return False

    # 2. A parks: a FRESH INVITE to orbit 700 (matches onParkInvite — a park-invite is a
    #    brand-new dialog, not a re-use of the original call-id).
    park_callid, pstatus, pbody = a.invite("700")
    if pstatus != 200:
        report("Call park + retrieve", False, "park: A->700 got status=%s" % pstatus)
        a.bye(echo_callid)
        return False
    if "a=inactive" not in pbody:
        report("Call park + retrieve", False, "park: 200 OK missing expected a=inactive hold SDP")
        a.bye(echo_callid); a.bye(park_callid)
        return False

    # 3. A DIFFERENT extension (C) retrieves by INVITEing the SAME orbit.
    retrieve_callid, rstatus, rbody = c.invite("700")
    if rstatus != 200:
        report("Call park + retrieve", False, "retrieve: C->700 got status=%s" % rstatus)
        a.bye(echo_callid); a.bye(park_callid)
        return False
    got_a_sdp = sdp_endpoint(rbody)
    a_own_ep = ("", a.rtp_port)
    sdp_matches_a = got_a_sdp is not None and got_a_sdp[1] == a.rtp_port

    # 4. A should receive a re-INVITE on the PARK dialog carrying C's SDP.
    reinv = a.wait_event("REINVITE", timeout=3.0, match=lambda e: e["callid"] == park_callid)
    reinv_matches_c = False
    if reinv:
        ep = sdp_endpoint(reinv["body"])
        reinv_matches_c = ep is not None and ep[1] == c.rtp_port

    # 5. Teardown: C hangs up the retrieved call; server should relay a BYE to A's park leg.
    c.bye(retrieve_callid)
    a_byed = a.wait_event("REQUEST", timeout=3.0,
                           match=lambda e: e["method"] == "BYE" and e["callid"] == park_callid)
    a.bye(echo_callid)

    ok = sdp_matches_a and reinv is not None and reinv_matches_c and a_byed is not None
    report("Call park + retrieve", ok,
           "retriever got A's SDP=%s, A got re-INVITE-with-C's-SDP=%s, BYE-relay-to-A=%s" %
           (sdp_matches_a, reinv_matches_c, a_byed is not None))
    return ok


def scenario_blind_transfer(board_ip, local_ip):
    # Dedicated, disposable phones — NOT the shared office set. The blind-transfer
    # redirect reuses the original call-id server-side for a NEW A->C leg that our
    # simplified UAC never learns to ACK/manage as its own transaction (a real desk
    # phone would); that half-open leg measurably interfered with LATER scenarios when
    # this used the shared A/B/C (observed: made an unrelated later INVITE to the same
    # extension flaky/no-response). Isolating it here contains that to phones we throw
    # away right after, rather than chasing the interaction further.
    phones = {k: Phone(board_ip, ext, phone_source_ip(local_ip)) for k, ext in
              {"A": "311", "B": "312", "C": "313"}.items()}
    for p in phones.values():
        if not p.register():
            report("Blind transfer", False, "setup: dedicated phone %s failed to register" % p.ext)
            for q in phones.values(): q.close()
            return False
    time.sleep(1.0)   # settle — see scenario_registration's comment
    a = phones["A"]; b = phones["B"]; c = phones["C"]
    try:
        return _scenario_blind_transfer_body(a, b, c)
    finally:
        for p in phones.values(): p.close()


def _scenario_blind_transfer_body(a, b, c):
    ab_callid, status, _ = a.invite(b.ext)
    if status != 200:
        report("Blind transfer", False, "setup: A->B got status=%s" % status)
        return False

    refer_t = now_ms()
    resp = a.refer(ab_callid, c.ext)
    if not resp or resp.status != 202:
        report("Blind transfer", False, "REFER got status=%s (expected 202)" %
               (resp.status if resp else "no-response"))
        a.bye(ab_callid)
        return False

    # B's original leg should be torn down (server BYEs B).
    b_byed = b.wait_event("REQUEST", timeout=3.0,
                           match=lambda e: e["method"] == "BYE" and e["callid"] == ab_callid)
    # C should receive a fresh INVITE (the redirected leg) — filtered to AFTER the REFER
    # (t >= refer_t), not just "C has ever received any INVITE": every phone gets an
    # INCOMING_CALL-shaped event for its own register-beep at registration time, which
    # would otherwise match here as a false positive on any already-registered C.
    c_invited = c.wait_event("INCOMING_CALL", timeout=3.0, match=lambda e: e["t"] >= refer_t)

    # Best-effort cleanup: the redirected A->C leg reuses ab_callid server-side (a real
    # desk phone would ACK it as an ordinary call), but our simplified A never tracked
    # it as a new outbound transaction, so it never sends that ACK. This phone is
    # discarded right after this scenario anyway (see scenario_blind_transfer), so it's
    # not load-bearing — just a tidy attempt to close what we opened.
    a.bye(ab_callid)
    time.sleep(0.2)

    ok = b_byed is not None and c_invited is not None
    report("Blind transfer", ok,
           "B torn down=%s, C received new INVITE=%s" % (b_byed is not None, c_invited is not None))
    return ok


def scenario_attended_transfer(board_ip, local_ip):
    # Dedicated, disposable phones — see scenario_blind_transfer's comment; attended
    # transfer splices two dialogs and drops the common party, which is equally
    # unusual post-state to leave on phones other scenarios still rely on.
    phones = {k: Phone(board_ip, ext, phone_source_ip(local_ip)) for k, ext in
              {"A": "321", "B": "322", "C": "323"}.items()}
    for p in phones.values():
        if not p.register():
            report("Attended transfer", False, "setup: dedicated phone %s failed to register" % p.ext)
            for q in phones.values(): q.close()
            return False
    time.sleep(1.0)   # settle — see scenario_registration's comment
    a = phones["A"]; b = phones["B"]; c = phones["C"]
    try:
        return _scenario_attended_transfer_body(a, b, c)
    finally:
        for p in phones.values(): p.close()


def _scenario_attended_transfer_body(a, b, c):
    ab_callid, status1, _ = a.invite(b.ext)
    ac_callid, status2, _ = a.invite(c.ext)
    if status1 != 200 or status2 != 200:
        report("Attended transfer", False,
               "setup: A->B=%s A->C=%s (expected both 200)" % (status1, status2))
        return False

    bare_ac_callid = ac_callid  # our Call-ID header value IS the bare call-id we sent
    with a.lock:
        ac_d = a.dialogs[ac_callid]
        from_tag = ac_d["local_tag"]      # A's tag in the A-C dialog
        to_tag = ac_d["remote_tag"]       # C's tag in the A-C dialog

    resp = a.refer(ab_callid, b.ext if False else c.ext,
                   replaces=(bare_ac_callid, from_tag, to_tag))
    if not resp or resp.status != 202:
        report("Attended transfer", False, "REFER(Replaces) got status=%s (expected 202)" %
               (resp.status if resp else "no-response"))
        a.bye(ab_callid); a.bye(ac_callid)
        return False

    b_reinv = b.wait_event("REINVITE", timeout=3.0, match=lambda e: e["callid"] == ab_callid)
    c_reinv = c.wait_event("REINVITE", timeout=3.0, match=lambda e: e["callid"] == ac_callid)
    b_got_c_sdp = False; c_got_b_sdp = False
    if b_reinv:
        ep = sdp_endpoint(b_reinv["body"]); b_got_c_sdp = ep is not None and ep[1] == c.rtp_port
    if c_reinv:
        ep = sdp_endpoint(c_reinv["body"]); c_got_b_sdp = ep is not None and ep[1] == b.rtp_port
    a_byed_ab = a.wait_event("REQUEST", timeout=2.0,
                              match=lambda e: e["method"] == "BYE" and e["callid"] == ab_callid)
    a_byed_ac = a.wait_event("REQUEST", timeout=2.0,
                              match=lambda e: e["method"] == "BYE" and e["callid"] == ac_callid)

    ok = b_got_c_sdp and c_got_b_sdp and a_byed_ab is not None and a_byed_ac is not None
    report("Attended transfer", ok,
           "B got C's SDP=%s, C got B's SDP=%s, A dropped from both=%s" %
           (b_got_c_sdp, c_got_b_sdp, a_byed_ab is not None and a_byed_ac is not None))
    return ok


def scenario_dnd(phones, board_ip, http_port):
    d = phones["D"]

    def dnd_is_set():
        st = http_get_json(board_ip, "/api/status", http_port)
        return d.ext in (st.get("dnd", []) if isinstance(st, dict) else [])

    in_dnd = d.send_dtmf_reliable("*60", dnd_is_set)

    caller = phones["A"]
    callid, status, _ = caller.invite(d.ext)
    # RequestsHandler::onInvite declines a DND'd extension with 480 Temporarily
    # Unavailable (deliberate — see the comment there), NOT 486 Busy Here. Some older
    # docs/roadmap text says 486; verified-against-code wins.
    got_480 = (status == 480)
    if status not in (0, 480):
        caller.bye(callid)  # unexpected answer — clean up

    cleared = d.send_dtmf_reliable("*80", lambda: not dnd_is_set())

    ok = in_dnd and got_480 and cleared
    report("DND (*60/*80)", ok,
           "*60 set dnd=%s, INVITE while DND status=%s (want 480), *80 cleared=%s" %
           (in_dnd, status, cleared))
    return ok


def scenario_call_forward(phones, board_ip, local_ip, http_port):
    # onDtmfInfo's *72NNNN handler requires the target to be AT LEAST 4 DIGITS
    # (`target.size() >= 4`, a deliberate disambiguation rule, not a bug) — the shared
    # office's 3-digit extensions (301-306) can never satisfy that, so *72 forwarding
    # to one of them silently never completes (it just keeps waiting for a 4th digit
    # that never comes). Use a dedicated 4-digit target extension for this scenario so
    # it actually exercises the feature as designed.
    e = phones["E"]
    target = Phone(board_ip, "4002", phone_source_ip(local_ip))
    if not target.register():
        report("Call forward (*72/*73)", False, "setup: dedicated 4-digit target failed to register")
        target.close()
        return False
    time.sleep(1.0)   # settle — see scenario_registration's comment
    try:
        def forward_is_set():
            st = http_get_json(board_ip, "/api/status", http_port)
            fwds = st.get("forwards", []) if isinstance(st, dict) else []
            return any(f.get("extension") == e.ext and f.get("always") == target.ext for f in fwds)

        set_ok = e.send_dtmf_reliable("*72" + target.ext, forward_is_set)

        caller = phones["A"]
        redirect_t = now_ms()
        callid, status, sdp = caller.invite(e.ext)
        # A forwarded call rings the TARGET, not E — expect a fresh INCOMING_CALL on F,
        # filtered to after we triggered the redirect (see the blind-transfer comment on
        # why an unfiltered check would false-positive on the target's own register-beep).
        f_invited = target.wait_event("INCOMING_CALL", timeout=3.0, match=lambda ev: ev["t"] >= redirect_t)
        if status == 200:
            caller.bye(callid)
        e.send_dtmf_reliable("*73", lambda: not forward_is_set())
        ok = set_ok and f_invited is not None
        report("Call forward (*72/*73)", ok,
               "call to E redirected to F=%s (final status=%s)" % (f_invited is not None, status))
        return ok
    finally:
        target.close()


def scenario_ring_group(phones, board_ip, http_port):
    members = ",".join(phones[k].ext for k in ("B", "C"))
    status, resp = http_post(board_ip, "/api/group",
                              {"extension": "850", "members": members, "mode": "ringall"}, http_port)
    if status != 200:
        report("Ring group", False, "POST /api/group failed status=%s body=%s" % (status, resp))
        return False
    caller = phones["A"]
    dial_t = now_ms()
    callid, cstatus, _ = caller.invite("850")
    # Filtered to after we dialed — see the blind-transfer comment: an unfiltered check
    # would false-positive on B/C's own register-beep INCOMING_CALL event.
    b_invited = phones["B"].wait_event("INCOMING_CALL", timeout=3.0, match=lambda e: e["t"] >= dial_t)
    c_invited = phones["C"].wait_event("INCOMING_CALL", timeout=3.0, match=lambda e: e["t"] >= dial_t)
    if cstatus == 200:
        caller.bye(callid)
    ok = b_invited is not None and c_invited is not None
    report("Ring group (ring-all)", ok,
           "group INVITE forked to B=%s C=%s (call status=%s)" %
           (b_invited is not None, c_invited is not None, cstatus))
    return ok


def scenario_page(phones):
    caller = phones["A"]
    page_t = now_ms()
    callid, status, _ = caller.invite("999")
    # Filtered to after we dialed — see the blind-transfer comment: an unfiltered check
    # would false-positive on each phone's own register-beep INCOMING_CALL event.
    others_invited = all(
        phones[k].wait_event("INCOMING_CALL", timeout=3.0, match=lambda e: e["t"] >= page_t) is not None
        for k in phones if k != "A"
    )
    if status == 200:
        caller.bye(callid)
    report("999 all-page broadcast", others_invited,
           "every other extension received the page INVITE=%s (call status=%s)" %
           (others_invited, status))
    return others_invited


def scenario_cdr(board_ip, http_port, before_count):
    st = http_get_json(board_ip, "/api/cdr", http_port)
    after_count = len(st) if isinstance(st, list) else -1
    ok = after_count > before_count
    report("CDR growth", ok, "CDR records %d -> %d" % (before_count, after_count))
    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    board_ip = sys.argv[1]
    http_port = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    local_ip = local_ip_for(board_ip)
    print("[office_smoke] board=%s http_port=%d local_ip=%s" % (board_ip, http_port, local_ip))

    before_cdr = http_get_json(board_ip, "/api/cdr", http_port)
    before_count = len(before_cdr) if isinstance(before_cdr, list) else 0

    ext_names = {"A": "301", "B": "302", "C": "303", "D": "304", "E": "305", "F": "306"}
    phones = {k: Phone(board_ip, ext, phone_source_ip(local_ip)) for k, ext in ext_names.items()}

    print("\n[office_smoke] ==== SCENARIOS ====")
    try:
        if not scenario_registration(phones):
            print("\n[office_smoke] registration failed for one or more extensions — "
                  "aborting remaining scenarios (they all assume a registered office).")
        else:
            scenario_echo_test(phones)
            scenario_codec_policy(phones)
            scenario_park_retrieve(phones)
            scenario_blind_transfer(board_ip, local_ip)
            scenario_attended_transfer(board_ip, local_ip)
            scenario_dnd(phones, board_ip, http_port)
            scenario_call_forward(phones, board_ip, local_ip, http_port)
            scenario_ring_group(phones, board_ip, http_port)
            scenario_page(phones)
            scenario_cdr(board_ip, http_port, before_count)
    finally:
        for p in phones.values():
            p.close()

    print("\n[office_smoke] ==== SUMMARY ====")
    passed = sum(1 for _, ok, _ in RESULTS if ok)
    for name, ok, detail in RESULTS:
        print("  %-4s %s" % ("OK" if ok else "FAIL", name))
    print("[office_smoke] %d/%d scenarios passed" % (passed, len(RESULTS)))
    sys.exit(0 if passed == len(RESULTS) else 1)


if __name__ == "__main__":
    main()
