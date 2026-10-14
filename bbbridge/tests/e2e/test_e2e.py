#!/usr/bin/env python3
"""End-to-end tests for the C++ bridge (bin/bbbridge-cli.exe).

The same checks the Python bridge passed (its test_offline.py and test_p2p.py),
run against the C++ build over real sockets: a fake bbhost server, a fake
STUN/relay server, a shadNet client, fake port and peer UDP sockets. The
protobuf / shadNet codecs used to read the bridge's answers are the Python
bridge's own (proto3.py, shadnet_proto.py) - an independent implementation.

    python tests/e2e/test_e2e.py [path\\to\\bbbridge-cli.exe]
"""
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proto3  # noqa: E402
import shadnet_proto as sp  # noqa: E402

EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "bin", "bbbridge-cli.exe")
HOST = "127.0.0.1"
NPID, HOST_NPID = "tester01", "hostplayer"
TIMEOUT = 5.0
PASSED, FAILED = [], []


def check(name, cond, detail=""):
    (PASSED if cond else FAILED).append((name, detail))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, "" if cond else "  " + str(detail)))


def account_id_for(npid):
    d = hashlib.sha256(("bbbridge:" + npid).encode()).digest()
    return (1 << 52) | (int.from_bytes(d[:8], "big") & ((1 << 48) - 1))


# ---------------------------------------------------------------- fake bbhost

class FakeBBHost(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        try:
            body = json.loads(self.rfile.read(n).decode()) if n else {}
        except ValueError:
            body = {}
        path = self.path.split("?")[0]
        st = self.server.state
        with st["lock"]:
            st["calls"].append(path)
            emit = st["emit_event"]
            event = st.get("event")
        r = {"ResKind": 0}
        if path == "/auth/account/create":
            r.update(Token="tok-123", OnlineId=body.get("Name"), RecoveryCode="RC-42")
        elif path == "/auth/recover":
            r.update(Token="tok-456", OnlineId=body.get("Name"), RecoveryCode="RC-43")
        elif path == "/mp/matching2/create_room":
            r.update(RoomId=777, SessionId="sess-A", MemberId=1, OwnerMemberId=1, MaxMembers=5, Members=[])
        elif path == "/mp/matching2/join_room":
            r.update(SessionId="sess-B", MemberId=2, OwnerMemberId=1, MaxMembers=5, Members=[
                {"MemberId": 1, "OnlineId": HOST_NPID, "LocalAddr": "10.0.0.9", "LocalPort": 5551,
                 "Addr": "9.9.9.8", "Port": 5552, "MappedAddr": "9.9.9.8", "MappedPort": 5552}])
        elif path == "/mp/matching2/heartbeat":
            r.update(InRoom=1)
        elif path == "/np/signaling/resolve":
            if self.headers.get("X-BB-Token") == "tok-expired":
                self._json({"ResKind": 1, "Message": "bad or expired token"}, 401)
                return
            r.update(Addr="9.9.9.9", Port=5555)
        elif path == "/auth/device/start":
            st["device_polls"] = 0
            r.update(DeviceCode="dev-1", UserCode="ABCD-EFGH", VerificationUri="http://example.test/link",
                     Interval=1, ExpiresIn=60, Device=body.get("DeviceName"))
        elif path == "/auth/device/poll":
            st["device_polls"] = st.get("device_polls", 0) + 1
            if body.get("DeviceCode") == "dev-1" and st["device_polls"] >= 2:
                r.update(Status="approved", Token="tok-discord", OnlineId="discorduser")
            else:
                r.update(Status="pending")
        elif path == "/auth/web_code":
            if body.get("Token") != "tok-discord":
                self._json({"ResKind": 1, "Message": "unknown token"})
                return
            r.update(Code="WXYZ-1234")
        elif path == "/np/events/poll":
            r.update(NextCursor=body.get("Cursor", 0) + 1, HasEvent=bool(emit))
            if emit:
                r.update(event or {"Name": "room_member_joined", "MemberId": 2, "OnlineId": HOST_NPID,
                                   "RoomId": 777, "LocalAddr": "10.0.0.9", "LocalPort": 5551,
                                   "Addr": "9.9.9.8", "Port": 5552, "MappedAddr": "9.9.9.8",
                                   "MappedPort": 5552})
        elif path == "/summon_messenger/create":
            r.update(MessageId="SummonDataCreateResponse")
            st["summon_token"] = self.headers.get("X-BB-Token", "")
        elif path == "/gs/echo":
            r.update(Token=self.headers.get("X-BB-Token", ""), UA=self.headers.get("User-Agent", ""), Body=body)
        elif path not in ("/mp/matching2/context_start", "/mp/matching2/leave_room", "/auth/logout",
                          "/mp/matching2/signaling_update", "/np/events/ack"):
            self._json({"ResKind": 1, "Message": "unknown path"}, 404)
            return
        self._json(r)


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass   # a bridge we terminate drops its keep-alive connections; that is expected


def fake_bbhost(port):
    srv = QuietServer((HOST, port), FakeBBHost)
    srv.state = {"lock": threading.Lock(), "calls": [], "emit_event": False, "event": None}
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---------------------------------------------------------------- bridge process

class ExeBridge:
    def __init__(self, cfg):
        self.dir = tempfile.mkdtemp(prefix="bbbridge-e2e-")
        self.cfg = dict(cfg, state_file=os.path.join(self.dir, "state.json"), debug_endpoints=True)
        path = os.path.join(self.dir, "config.json")
        with open(path, "w") as fh:
            json.dump(self.cfg, fh)
        self.log = os.path.join(self.dir, "bridge.log")
        self.proc = subprocess.Popen([EXE, "--config", path, "--log", self.log],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://%s:%d/status" % (HOST, self.cfg["webapi_port"]), timeout=1).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("bridge did not start; log: %s" % self.log)

    def _url(self, path):
        return "http://%s:%d%s" % (HOST, self.cfg["webapi_port"], path)

    def debug(self):
        with urllib.request.urlopen(self._url("/debug/state"), timeout=TIMEOUT) as r:
            return json.loads(r.read())

    def post(self, path, obj):
        req = urllib.request.Request(self._url(path), data=json.dumps(obj).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return json.loads(r.read())

    def peer(self, online_id):
        return next((p for p in self.debug()["peers"] if p["online_id"] == online_id), None)

    def stop(self):
        self.proc.terminate()
        self.proc.wait(10)


class ShadNetClient:
    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=TIMEOUT)
        self.next_id = 1
        self.queue = []
        ptype, _, _, payload = self._read()
        assert ptype == sp.PACKET_SERVER_INFO and struct.unpack("<I", payload)[0] == sp.PROTOCOL_VERSION

    def _exact(self, n):
        buf = b""
        while len(buf) < n:
            c = self.sock.recv(n - len(buf))
            if not c:
                raise ConnectionError("closed")
            buf += c
        return buf

    def _read(self):
        ptype, cmd, size, pid = sp.parse_header(self._exact(sp.HEADER_SIZE))
        return ptype, cmd, pid, self._exact(size - sp.HEADER_SIZE) if size > sp.HEADER_SIZE else b""

    def request(self, command, msg=None):
        payload = sp.proto_blob(msg) if msg is not None else b""
        self.sock.sendall(sp.build_packet(sp.PACKET_REQUEST, command, self.next_id, payload))
        self.next_id += 1
        deadline = time.time() + TIMEOUT * 2
        while time.time() < deadline:
            ptype, cmd, _, raw = self._read()
            if ptype == sp.PACKET_REPLY and cmd == command:
                return (raw[0] if raw else 255), sp.parse_proto_blob(raw[1:])
            self.queue.append((ptype, cmd, raw))
        raise TimeoutError(command)

    def notification(self, kind, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for i, (ptype, cmd, raw) in enumerate(self.queue):
                if ptype == sp.PACKET_NOTIFICATION and cmd == kind:
                    return self.queue.pop(i)[2]
            try:
                self.sock.settimeout(0.5)
                ptype, cmd, _, raw = self._read()
            except (socket.timeout, TimeoutError):
                continue
            finally:
                self.sock.settimeout(TIMEOUT)
            self.queue.append((ptype, cmd, raw))
        return None

    def close(self):
        self.sock.close()


def stun_ping(npid, udp_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(TIMEOUT)
    s.sendto(b"\x01" + npid.encode()[:16].ljust(16, b"\0") + socket.inet_aton(HOST), (HOST, udp_port))
    data, _ = s.recvfrom(64)
    port = s.getsockname()[1]
    s.close()
    return socket.inet_ntoa(data[:4]), struct.unpack(">H", data[4:6])[0], port


def get_json(url):
    with urllib.request.urlopen(url, timeout=TIMEOUT) as r:
        return r.status, json.loads(r.read())


def post_json(url, obj, headers=None):
    req = urllib.request.Request(url, data=json.dumps(obj).encode(), method="POST",
                                 headers=dict({"Content-Type": "application/json"}, **(headers or {})))
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read())


# ---------------------------------------------------------------- A: server path

def part_a():
    print("-- A: server path (gateway off)")
    ports = {"tcp": 42313, "udp": 42314, "webapi": 42315, "bbhost": 42671, "gs": 42316}
    fake = fake_bbhost(ports["bbhost"])
    up = "http://%s:%d" % (HOST, ports["bbhost"])
    br = ExeBridge({"bbhost_server": up, "community_server": up, "bind_host": HOST,
                    "tcp_port": ports["tcp"], "udp_port": ports["udp"], "webapi_port": ports["webapi"],
                    "p2p_gateway": False, "local_addr": HOST, "poll_interval": 0.05,
                    "game_proxy_routes": {str(ports["gs"]): up}, "create_accounts": True})
    c = None
    try:
        c = ShadNetClient(ports["tcp"])
        err, blob = c.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: NPID, 4: "CUSA03173"}))
        reply = proto3.decode_message(sp.LOGIN_REPLY, blob) if blob else {}
        check("login accepted with the per-player account id",
              err == 0 and reply.get("user_id") == account_id_for(NPID), (err, reply))
        check("bbhost account created", "/auth/account/create" in fake.state["calls"])
        err, blob = c.request(sp.Command.GetServerFeatures)
        check("matching2 enabled", err == 0 and proto3.decode_message(sp.SERVER_FEATURES_REPLY, blob).get("matching2_enabled") is True)
        err, blob = c.request(sp.Command.GetToken)
        tok = proto3.decode_message(sp.GET_TOKEN_REPLY, blob)
        check("token relayed", err == 0 and tok.get("token") == "tok-123" and tok.get("npid") == NPID, tok)

        ip, port, local = stun_ping(NPID, ports["udp"])
        check("stun reply", ip == HOST and port == local, (ip, port, local))
        time.sleep(0.2)
        check("stun registry", br.debug()["registry"].get(NPID) == [HOST, local], br.debug()["registry"])

        err, _ = c.request(sp.Command.ContextStart, proto3.encode_message(sp.CONTEXT_START_REQUEST, {1: 7}))
        check("context_start", err == 0 and "/mp/matching2/context_start" in fake.state["calls"], err)

        err, blob = c.request(sp.Command.CreateRoom, proto3.encode_message(sp.CREATE_ROOM_REQUEST, {1: 1, 2: 5}))
        reply = proto3.decode_message(sp.CREATE_ROOM_REPLY, blob) if blob else {}
        check("create_room accepted", err == 0 and reply.get("room_id") == 777 and reply.get("member_id") == 1
              and reply.get("max_slots") == 5, (err, reply))
        details = reply.get("details") or {}
        check("create_room details", details.get("me_member_id") == 1 and details.get("owner_member_id") == 1
              and (details.get("room_data") or {}).get("room_id") == 777, details)
        me = [m for m in details.get("members") or [] if m.get("member_id") == 1]
        check("create_room lists ourselves (port needs members.me/owner)",
              len(me) == 1 and me[0].get("npid") == NPID and me[0].get("is_owner") is True, details.get("members"))

        with fake.state["lock"]:
            fake.state["emit_event"] = True
        raw = c.notification(sp.Notify.RoomEvent)
        with fake.state["lock"]:
            fake.state["emit_event"] = False
        ev = proto3.decode_message(sp.NOTIFY_ROOM_EVENT, sp.parse_proto_blob(raw)) if raw else {}
        check("room_member_joined notification", ev.get("event") == sp.RoomEvent.MemberJoined
              and ev.get("room_id") == 777 and (ev.get("member") or {}).get("npid") == HOST_NPID
              and (ev.get("member") or {}).get("member_id") == 2, ev)

        err, blob = c.request(sp.Command.RequestSignalingInfos,
                              proto3.encode_message(sp.REQUEST_SIGNALING_INFOS_REQUEST, {1: HOST_NPID}))
        sig = proto3.decode_message(sp.REQUEST_SIGNALING_INFOS_REPLY, blob) if blob else {}
        check("signaling resolve", err == 0 and sig.get("target_ip") == "9.9.9.9" and sig.get("target_port") == 5555, sig)

        webapi = "http://%s:%d" % (HOST, ports["webapi"])
        check("webapi /status", get_json(webapi + "/status")[1].get("ok") is True)
        ok = True
        for name in ("friendList", "blockList"):
            status, body = get_json("%s/v1/users/%s/%s?fields=npId&limit=50" % (webapi, NPID, name))
            ok = ok and status == 200 and body.get(name) == [] and body.get("totalResults") == 0
        check("webapi friendList/blockList answer empty lists", ok)
        ad = {"SessionId": "sess-A", "UserId": 1, "AreaId": 3, "SummonData": "QUJD", "MatchingLevel": 44}
        r = post_json(webapi + "/summon_messenger/create", ad)
        check("summon create proxied", r.get("ResKind") == 0 and r.get("MessageId") == "SummonDataCreateResponse", r)
        check("summon proxy carries the account token", fake.state.get("summon_token") == "tok-123",
              fake.state.get("summon_token"))
        echo = post_json("http://%s:%d/gs/echo" % (HOST, ports["gs"]), {"x": 1}, {"User-Agent": "BloodborneGame"})
        check("game proxy forwards with token and own UA", echo.get("Token") == "tok-123" and echo.get("Body") == {"x": 1}
              and echo.get("UA", "").startswith("BloodborneGame bbbridge/"), echo)
        check("advertisement captured", (br.debug().get("last_advertisement") or {}).get("AreaId") == 3)

        err, blob = c.request(sp.Command.LeaveRoom, proto3.encode_message(sp.LEAVE_ROOM_REQUEST, {1: 777, 2: 2}))
        check("leave_room accepted", err == 0 and proto3.decode_message(sp.LEAVE_ROOM_REPLY, blob).get("room_id") == 777
              and "/mp/matching2/leave_room" in fake.state["calls"])
        err, _ = c.request(sp.Command.Terminate)
        closed = False
        try:
            c.sock.settimeout(2)
            closed = not c.sock.recv(16)
        except OSError:
            closed = True
        check("terminate closes the connection", err == 0 and closed)

        c2 = ShadNetClient(ports["tcp"])
        err, _ = c2.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: NPID}))
        _, blob = c2.request(sp.Command.GetToken)
        c2.close()
        check("second login uses the stored token, no recover",
              err == 0 and fake.state["calls"].count("/auth/recover") == 0
              and proto3.decode_message(sp.GET_TOKEN_REPLY, blob).get("token") == "tok-123")
    finally:
        if c:
            c.close()
        br.stop()
        fake.shutdown()
        fake.server_close()


# ---------------------------------------------------------------- B: gateway

class FakeStun(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((HOST, 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.relay_port = 41000
        self.token = b"RELAYTOK"
        self.requests, self.relayed = [], []
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                data, src = self.sock.recvfrom(65536)
            except (socket.timeout, OSError):
                continue
            if data[:2] == b"\xfbR":
                self.relayed.append(data)
                continue
            if len(data) < 20 or struct.unpack(">H", data[:2])[0] != 1:
                continue
            self.requests.append(data)
            attrs = struct.pack(">HHBBH4s", 0x0001, 8, 0, 1, 5678, socket.inet_aton("1.2.3.4"))
            if self.relay_port and len(data) >= 36 and data[20:22] == b"\x81\x00":
                v = b"bbr1" + self.token + struct.pack(">H", self.relay_port) + socket.inet_aton(src[0]) + struct.pack(">H", src[1])
                attrs += struct.pack(">HH", 0x8101, len(v)) + v
            self.sock.sendto(struct.pack(">HH", 0x0101, len(attrs)) + data[4:20] + attrs, src)


def udp(timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, 0))
    s.settimeout(timeout)
    return s


def recv_until(sock, pred, timeout=2.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.05, deadline - time.time()))
        try:
            data, src = sock.recvfrom(65536)
        except socket.timeout:
            return None
        if pred(data):
            return data, src
    return None


def handshake(kind, frm, to, from_conn=7, nonce=0):
    return b"SHAD" + bytes([0x07, kind]) + frm.encode().ljust(16, b"\0") + to.encode().ljust(16, b"\0") + \
        struct.pack("<IIIHHQ", from_conn, 0, 0, 0, 0, nonce)


def m2(kind, room, frm_member, to_member, frm_id, nonce=0):
    return b"SHAD" + bytes([0x21, kind]) + struct.pack("<QHH", room, frm_member, to_member) + \
        frm_id.encode().ljust(16, b"\0") + struct.pack("<IHHQ", 0, 0, 0, nonce)


def part_b():
    print("-- B: gateway over real sockets")
    stun = FakeStun()
    stun.start()
    ports = {"tcp": 43313, "udp": 43314, "webapi": 43315}
    br = ExeBridge({"bbhost_server": "http://%s:1" % HOST, "bind_host": HOST, "tcp_port": ports["tcp"],
                    "udp_port": ports["udp"], "webapi_port": ports["webapi"], "local_addr": HOST,
                    "stun_server": "%s:%d" % (HOST, stun.port), "stun_refresh": 0.3, "game_proxy_routes": {}})
    port_sock, peer_sock = udp(), udp()
    try:
        deadline = time.time() + 4
        while time.time() < deadline and not (br.debug().get("mapped") and br.debug().get("relay_port")):
            time.sleep(0.1)
        st = br.debug()
        check("STUN request carries BBHOST-HELLO", bool(stun.requests) and stun.requests[0][20:24] == b"\x81\x00\x00\x0c")
        check("STUN mapping learned", st.get("mapped") == ["1.2.3.4", 5678], st.get("mapped"))
        check("relay turned on", st.get("relay_port") == 41000, st.get("relay_port"))

        stun.relay_port = 0   # direct part: the next refresh turns the relay off
        deadline = time.time() + 3
        while time.time() < deadline and br.debug().get("relay_port"):
            time.sleep(0.1)
        inner = tuple(br.post("/debug/add_peer", {
            "member_id": 2, "online_id": "hunter", "endpoint": list(peer_sock.getsockname()),
            "port_online_id": "porthost", "port_member_id": 1, "room_id": 777,
            "port_endpoint": list(port_sock.getsockname())})["inner"])
        check("inner endpoint on loopback", inner[0] == HOST and inner[1] > 0, inner)
        opening = recv_until(port_sock, lambda d: d[4:9] == b"SHAD\x10")
        check("peer opens with ActivationRequest to the port",
              opening is not None and opening[0][9] == 1 and opening[1] == inner, opening)

        port_sock.sendto(struct.pack(">HH", 40, 30) + b"hello-peer", inner)
        got = recv_until(peer_sock, lambda d: d[:1] == b"\xff")
        check("port game data reaches the peer in bbhost framing",
              got is not None and got[0] == b"\xff\xc3\x28\x1ehello-peer", got)
        outer_port = got[1][1] if got else 0
        peer_sock.sendto(b"\xff\xc3\x1e\x28hello-port", (HOST, outer_port))
        got = recv_until(port_sock, lambda d: d.endswith(b"hello-port"))
        check("peer game data reaches the port in port framing",
              got is not None and got[0] == struct.pack(">HH", 30, 40) + b"hello-port" and got[1] == inner, got)
        peer_sock.sendto(b"\xfebbhp\0\0\0", (HOST, outer_port))
        got = recv_until(port_sock, lambda d: d[:4] != b"\xff\xff\xff\xff", timeout=0.4)
        check("hole-punch probe not delivered to the port", got is None, got)

        sig = struct.pack(">HH", 0xFFFF, 0xFFFF)
        port_sock.sendto(sig + handshake(1, "porthost", "hunter"), inner)
        check("port's handshake Offer answered with Accept",
              recv_until(port_sock, lambda d: d[4:10] == b"SHAD\x07\x02") is not None)
        port_sock.sendto(sig + m2(3, 777, 1, 2, "porthost", nonce=42), inner)
        got = recv_until(port_sock, lambda d: d[4:10] == b"SHAD\x21\x04")
        check("port's matching2 Check answered with CheckAck",
              got is not None and struct.unpack("<Q", got[0][4 + 42:4 + 50])[0] == 42)

        stun.relay_port = 41000
        deadline = time.time() + 3
        while time.time() < deadline and not br.debug().get("relay_port"):
            time.sleep(0.1)
        inner3 = tuple(br.post("/debug/add_peer", {
            "member_id": 3, "online_id": "relayed", "endpoint": [HOST, 41001], "port_online_id": "porthost",
            "port_member_id": 1, "room_id": 777, "port_endpoint": list(port_sock.getsockname())})["inner"])
        port_sock.sendto(struct.pack(">HH", 40, 30) + b"via-relay", inner3)
        deadline = time.time() + 2
        while time.time() < deadline and not stun.relayed:
            time.sleep(0.05)
        expect = b"\xfbR" + stun.token + struct.pack(">H", 41001) + b"\xff\xc3\x28\x1evia-relay"
        check("datagram for a relayed peer goes framed to the STUN port", stun.relayed[:1] == [expect], stun.relayed[:1])
        stun.sock.sendto(b"\xfbr" + struct.pack(">H", 41001) + b"\xff\xc3\x1e\x28back", (HOST, outer_port))
        got = recv_until(port_sock, lambda d: d.endswith(b"back"))
        check("relay delivery reaches the port from the right member", got is not None and got[1] == inner3, got)

        br.post("/debug/remove_peer", {"member_id": 2})
        check("removed peer forgotten", br.peer("hunter") is None)
    finally:
        stun.stop = True
        br.stop()
        port_sock.close()
        peer_sock.close()


# ---------------------------------------------------------------- C: bridge + gateway

def part_c():
    print("-- C: bridge flows with the gateway on")
    ports = {"tcp": 44313, "udp": 44314, "webapi": 44315, "bbhost": 44671}
    fake = fake_bbhost(ports["bbhost"])
    up = "http://%s:%d" % (HOST, ports["bbhost"])
    br = ExeBridge({"bbhost_server": up, "community_server": up, "bind_host": HOST, "tcp_port": ports["tcp"],
                    "udp_port": ports["udp"], "webapi_port": ports["webapi"], "local_addr": HOST,
                    "poll_interval": 0.05, "stun_server": "off", "game_proxy_routes": {}, "create_accounts": True})
    c = None
    try:
        c = ShadNetClient(ports["tcp"])
        c.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: NPID}))
        stun_ping(NPID, ports["udp"])
        c.request(sp.Command.ContextStart, proto3.encode_message(sp.CONTEXT_START_REQUEST, {1: 7}))
        with fake.state["lock"]:
            fake.state["event"] = {"Name": "guest_invite", "RoomId": 777, "HostOnlineId": HOST_NPID,
                                   "HostAddr": "9.9.9.8", "HostPort": 5552, "HostLocalAddr": "10.0.0.9",
                                   "HostLocalPort": 5551, "HostMappedAddr": "9.9.9.8", "HostMappedPort": 5552,
                                   "HostArea": 3, "HostLevel": 26}
            fake.state["emit_event"] = True
        deadline = time.time() + 3
        while time.time() < deadline and br.peer(HOST_NPID) is None:
            time.sleep(0.05)
        with fake.state["lock"]:
            fake.state["emit_event"] = False
            fake.state["event"] = None
        seeded = br.peer(HOST_NPID)
        check("guest_invite seeds the host in the gateway", seeded is not None)

        err, blob = c.request(sp.Command.JoinRoom, proto3.encode_message(sp.JOIN_ROOM_REQUEST, {1: 777, 2: 1}))
        reply = proto3.decode_message(sp.JOIN_ROOM_REPLY, blob) if blob else {}
        host = next((m for m in (reply.get("details") or {}).get("members") or [] if m.get("npid") == HOST_NPID), {})
        link = br.peer(HOST_NPID)
        check("joining the invited room keeps the host's inner endpoint",
              seeded is not None and link is not None and link["inner"] == seeded["inner"], (seeded, link))
        check("the join fills in the port's member id for the handshakes",
              link is not None and link["port_member_id"] == 2 and link["room_id"] == 777, link)
        check("joined room member points at the gateway",
              err == 0 and link is not None and host.get("addr") == HOST and host.get("port") == link["inner"][1], host)
        err, blob = c.request(sp.Command.RequestSignalingInfos,
                              proto3.encode_message(sp.REQUEST_SIGNALING_INFOS_REQUEST, {1: HOST_NPID}))
        sig = proto3.decode_message(sp.REQUEST_SIGNALING_INFOS_REPLY, blob) if blob else {}
        check("signaling infos point at the gateway",
              err == 0 and link is not None and sig.get("target_ip") == HOST and sig.get("target_port") == link["inner"][1], sig)

        err, blob = c.request(sp.Command.RequestSignalingInfos,
                              proto3.encode_message(sp.REQUEST_SIGNALING_INFOS_REQUEST, {1: "summonedguest"}))
        sig2 = proto3.decode_message(sp.REQUEST_SIGNALING_INFOS_REPLY, blob) if blob else {}
        early = br.peer("summonedguest")
        check("signaling infos for a non-member resolve through the server",
              err == 0 and early is not None and sig2.get("target_port") == early["inner"][1], (sig2, early))
        with fake.state["lock"]:
            fake.state["event"] = {"Name": "room_member_joined", "MemberId": 3, "RoomId": 777,
                                   "OnlineId": "summonedguest", "Addr": "9.9.9.9", "Port": 5555}
            fake.state["emit_event"] = True
        deadline = time.time() + 3
        while time.time() < deadline and (br.peer("summonedguest") or {}).get("member_id") != 3:
            time.sleep(0.05)
        with fake.state["lock"]:
            fake.state["emit_event"] = False
            fake.state["event"] = None
        joined = br.peer("summonedguest")
        check("the guest joining keeps the inner endpoint the port already uses",
              joined is not None and early is not None and joined["member_id"] == 3 and joined["inner"] == early["inner"]
              and joined["peer_member_id"] == 3 and all(p["member_id"] > 0 for p in br.debug()["peers"]), joined)

        time.sleep(5.6)
        check("heartbeat inside the server's 15 s window", fake.state["calls"].count("/mp/matching2/heartbeat") >= 1)
        c.request(sp.Command.LeaveRoom, proto3.encode_message(sp.LEAVE_ROOM_REQUEST, {1: 777}))
        check("leaving the room drops the peers", br.debug()["peers"] == [], br.debug()["peers"])
    finally:
        if c:
            c.close()
        br.stop()
        fake.shutdown()
        fake.server_close()


# ---------------------------------------------------------------- D: Discord sign-in

def part_d():
    print("-- D: sign in with Discord, website code, expired token")
    port = 45671
    fake = fake_bbhost(port)
    up = "http://%s:%d" % (HOST, port)
    d = tempfile.mkdtemp(prefix="bbbridge-e2e-")
    state = os.path.join(d, "state.json")
    cfg_path = os.path.join(d, "config.json")
    with open(cfg_path, "w") as fh:
        json.dump({"auth_server": up, "bbhost_server": up, "state_file": state}, fh)
    try:
        r = subprocess.run([EXE, "--config", cfg_path, "--link-discord"], capture_output=True, text=True, timeout=30)
        check("device code shown to the player", "ABCD-EFGH" in r.stdout and "http://example.test/link" in r.stdout,
              r.stdout)
        check("approval signs in", r.returncode == 0 and "signed in as discorduser" in r.stdout, r.stdout + r.stderr)
        with open(state) as fh:
            acc = json.load(fh)["accounts"].get("discorduser", {})
        check("Discord account stored under its name",
              acc.get("token") == "tok-discord" and acc.get("linked") == "discord" and not acc.get("recovery_code"), acc)
        r = subprocess.run([EXE, "--config", cfg_path, "--web-code", "discorduser"], capture_output=True, text=True,
                           timeout=30)
        check("website sign-in code", r.returncode == 0 and r.stdout.strip() == "WXYZ-1234", r.stdout + r.stderr)

        def cli(*args):
            return subprocess.run([EXE, "--config", cfg_path, *args], capture_output=True, text=True, timeout=30)

        def accounts():
            with open(state) as fh:
                return json.load(fh)["accounts"]

        r = cli("--create", "pcplayer")
        check("create account on this PC", r.returncode == 0 and accounts().get("pcplayer", {}).get("recovery_code") == "RC-42"
              and accounts()["pcplayer"].get("token") == "tok-123", r.stdout + str(accounts().get("pcplayer")))
        r = cli("--recover", "pcplayer", "RC-42")
        check("recover account keeps the server's new code", r.returncode == 0
              and accounts()["pcplayer"].get("recovery_code") == "RC-43" and accounts()["pcplayer"].get("token") == "tok-456",
              accounts().get("pcplayer"))
        r = cli("--sign-out", "pcplayer")
        check("sign out ends the token, keeps the recovery code", r.returncode == 0 and "/auth/logout" in fake.state["calls"]
              and "token" not in accounts()["pcplayer"] and accounts()["pcplayer"].get("recovery_code") == "RC-43",
              accounts().get("pcplayer"))
        r = cli("--sign-out", "discorduser")
        check("signing out of a Discord account forgets it here", r.returncode == 0 and "discorduser" not in accounts())
        acc = dict(acc)   # the bridge part below uses the Discord account as linked earlier
        creates = fake.state["calls"].count("/auth/account/create")   # the bridge must add none below

        br = ExeBridge({"bbhost_server": up, "community_server": up, "auth_server": up, "bind_host": HOST,
                        "tcp_port": 45313, "udp_port": 45314, "webapi_port": 45315, "p2p_gateway": False,
                        "local_addr": HOST, "game_proxy_routes": {}})
        try:
            with open(br.cfg["state_file"], "w") as fh:
                json.dump({"accounts": {"discorduser": acc, "expired": {
                    "bbhost_name": "expired", "token": "tok-expired", "online_id": "expired", "linked": "discord"}}}, fh)
            c = ShadNetClient(45313)
            err, _ = c.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: "discorduser"}))
            _, blob = c.request(sp.Command.GetToken)
            c.close()
            check("the bridge plays as the Discord account",
                  err == 0 and proto3.decode_message(sp.GET_TOKEN_REPLY, blob).get("token") == "tok-discord"
                  and fake.state["calls"].count("/auth/account/create") == creates)
            c = ShadNetClient(45313)
            err, _ = c.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: "expired"}))
            c.close()
            with open(br.log) as fh:
                said = "has expired" in fh.read()
            check("an expired Discord sign-in is refused, not replaced by a new account",
                  err == sp.Error.LoginError and said and fake.state["calls"].count("/auth/account/create") == creates, err)
            c = ShadNetClient(45313)
            err, _ = c.request(sp.Command.Login, proto3.encode_message(sp.LOGIN_REQUEST, {1: "stranger"}))
            c.close()
            with open(br.log) as fh:
                said = "use 'Sign in with Discord' in bbbridge first" in fh.read()
            check("no account on this PC: sign in with Discord first, nothing created",
                  err == sp.Error.LoginError and said and fake.state["calls"].count("/auth/account/create") == creates, err)
        finally:
            br.stop()
    finally:
        fake.shutdown()
        fake.server_close()


# ---------------------------------------------------------------- E: bbport setup

def part_e():
    print("-- E: routing a fresh bbport (sandboxed APPDATA)")
    tmp = tempfile.mkdtemp(prefix="bbbridge-e2e-")
    appdata, bbport = os.path.join(tmp, "AppData"), os.path.join(tmp, "bloodborne")
    user = os.path.join(bbport, "user")
    os.makedirs(os.path.join(appdata, "bbport-launcher"))
    os.makedirs(user)
    open(os.path.join(bbport, "Bloodborne.exe"), "wb").close()
    mine = {"https://example.test:1": "http://127.0.0.1:1"}
    with open(os.path.join(user, "host_overrides.custom.json"), "w") as fh:
        json.dump(mine, fh)
    sfile = os.path.join(appdata, "bbport-launcher", "settings.json")
    fresh = {"ui_language": "", "game_dir": os.path.join(bbport, "CUSA03173"), "user_dir": "", "online": False,
             "online_server": "srv.shadps4.net:31313", "online_webapi": "", "online_npid": "", "online_password": "",
             "online_upnp": True, "extra_env": "BB_GPU_LOG=info SHADPS4_P2P_PORT=1"}
    with open(sfile, "w") as fh:
        json.dump(fresh, fh)
    env = dict(os.environ, APPDATA=appdata)

    def route(on):
        return subprocess.run([EXE, "--route", "on" if on else "off", "--bbport", bbport], env=env,
                              capture_output=True, text=True, timeout=30)

    def settings():
        with open(sfile) as fh:
            return json.load(fh)

    r = route(True)
    s = settings()
    check("route on: bbport points at the bridge", r.returncode == 0 and "routed=1" in r.stdout and s["online"] is True
          and s["online_server"] == "127.0.0.1:31313" and s["online_webapi"] == "http://127.0.0.1:31315", r.stdout + str(s))
    check("route on: an empty password gets a placeholder (bbport skips online without one)",
          s["online_password"] == "bbbridge", s["online_password"])
    check("route on: extra variables merged, P2P port pinned once",
          s["extra_env"].split() == ["BB_GPU_LOG=info", "SHADPS4_P2P_PORT=3658"], s["extra_env"])
    check("route on: npid untouched, key order kept", s["online_npid"] == "" and list(s)[:4] == list(fresh)[:4], list(s))
    ov = json.load(open(os.path.join(user, "host_overrides.custom.json")))
    check("route on: overrides written, the player's own backed up",
          "_bbbridge" in ov and json.load(open(os.path.join(user, "host_overrides.custom.json.before-bbbridge"))) == mine)
    r = route(False)
    s = settings()
    check("route off: back to shadNet, password and variables restored",
          r.returncode == 0 and s["online_server"] == "srv.shadps4.net:31313" and s["online_webapi"] == ""
          and s["online_password"] == "" and s["extra_env"] == "BB_GPU_LOG=info", s)
    check("route off: the player's own overrides restored",
          json.load(open(os.path.join(user, "host_overrides.custom.json"))) == mine)
    s["online_password"] = "secret"
    with open(sfile, "w") as fh:
        json.dump(s, fh)
    route(True)
    on_pw = settings()["online_password"]
    route(False)
    check("a real shadNet password is left alone", on_pw == "secret" and settings()["online_password"] == "secret",
          (on_pw, settings()["online_password"]))

    def set_settings(**kv):
        s = settings()
        s.update(kv)
        with open(sfile, "w") as fh:
            json.dump(s, fh)

    # the Python bbbridge's file is ours too: replaced, then removed - not kept as the player's
    ovfile = os.path.join(user, "host_overrides.custom.json")
    with open(ovfile, "w") as fh:
        json.dump({"_note": "bbbridge: the game's own server traffic goes through the bridge",
                   "https://thehuntersdream.com:20443": "http://127.0.0.1:31316"}, fh)
    route(True)
    route(False)
    check("route off: an old (Python) bbbridge overrides file is removed, not restored",
          not os.path.exists(ovfile) and not os.path.exists(ovfile + ".before-bbbridge"),
          os.listdir(user))

    # the app puts the Hunter's Dream account in while routed; off brings the shadNet one back
    for name, pw in (("hunter_sn", "secret"), ("renamed_sn", "secret2")):   # also when changed in between
        set_settings(online_npid=name, online_password=pw)
        route(True)
        set_settings(online_npid="discord_name")
        route(False)
        s = settings()
        check("route off: the shadNet account comes back (%s)" % name,
              s["online_npid"] == name and s["online_password"] == pw and s["online_server"] == "srv.shadps4.net:31313",
              (s["online_npid"], s["online_server"]))


def main():
    print("== bbbridge C++ end-to-end (%s) ==" % os.path.abspath(EXE))
    part_a()
    part_b()
    part_c()
    part_d()
    part_e()
    print("\n%d passed, %d failed" % (len(PASSED), len(FAILED)))
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
