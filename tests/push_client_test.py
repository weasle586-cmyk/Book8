#!/usr/bin/env python3
"""Host test for book8_push.py.

  python3 tests/push_client_test.py [build/push_interop_host]

Always checks packet layout, the kernel-image precheck and the one-shot TFTP
server over loopback (including a lost ACK). With the interop harness (built by
build.sh from tests/push_interop_host.c and the real net/push.c agent) it also
checks that client-built packets are accepted and agent replies authenticate.
"""
import hashlib
import os
import socket
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import book8_push as bp  # noqa: E402

KEY = bytes(range(1, 33))
CLIENT_PORT = 50000


def check(cond, message):
    if not cond:
        print(f"FAIL: {message}")
        sys.exit(1)


def test_layout():
    nonce = bytes(range(16))
    hello = bp.build_hello(0x01020304, nonce)
    check(hello == bytes.fromhex("424b385001010000" "01020304") + nonce, "HELLO bytes")
    cmd = bp.build_command(KEY, 7, nonce, bp.CONFIG, flags=1, ip=0xC0A832D0,
                           size=5, remote="a/b.bin", local="X.ELF")
    check(len(cmd) == 196 and cmd[5] == bp.COMMAND and cmd[6:8] == b"\x00\x88", "COMMAND hdr")
    check(cmd[28] == bp.CONFIG and cmd[29] == 1 and cmd[32:36] == b"\xc0\xa8\x32\xd0",
          "COMMAND body")
    check(cmd[28 + 56:28 + 63] == b"a/b.bin" and cmd[28 + 120:28 + 125] == b"X.ELF", "strings")
    check(cmd[164:] == bp.mac(KEY, cmd[:164]), "COMMAND MAC")
    for bad in ("has space", "x" * 64):
        try:
            bp.build_command(KEY, 1, nonce, bp.FILE, remote=bad)
        except bp.PushError:
            continue
        check(False, f"accepted remote {bad!r}")
    check(bp.valid_83("HELLO.ELF") and bp.valid_83("BOOK8NET") and not bp.valid_83("TOOLONGNAME.ELF")
          and not bp.valid_83("a.elf"), "8.3 names")
    check(bp.relaunch_count("build 5 up 3s relaunch 2 config FILE telemetry-failures 0") == 2,
          "relaunch count parse")


def test_kernel_check():
    good = bytearray(4096)
    good[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", good, 18, 0x3E)
    struct.pack_into("<IIII", good, 1024, bp.MB2_MAGIC, 0, 24,
                     (-(bp.MB2_MAGIC + 24)) & 0xFFFFFFFF)
    bp.check_kernel_image(bytes(good))
    for mutate in (lambda b: b.__setitem__(slice(1024, 1028), b"\0\0\0\0"),
                   lambda b: b.__setitem__(4, 1),
                   lambda b: struct.pack_into("<H", b, 18, 3)):
        bad = bytearray(good)
        mutate(bad)
        try:
            bp.check_kernel_image(bytes(bad))
        except bp.PushError:
            continue
        check(False, "accepted a bad kernel image")
    elf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "book8.elf")
    if os.path.exists(elf):
        with open(elf, "rb") as f:
            bp.check_kernel_image(f.read())


def tftp_fetch(port, name, drop_ack_for=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(5)
    sock.sendto(struct.pack("!H", 1) + name.encode() + b"\0octet\0", ("127.0.0.1", port))
    out, expected, dropped = bytearray(), 1, False
    while True:
        packet, server = sock.recvfrom(1024)
        opcode, block = struct.unpack_from("!HH", packet)
        check(opcode == 3, f"TFTP opcode {opcode}")
        if block == expected:
            if block == drop_ack_for and not dropped:
                dropped = True
                continue           # lose this ACK; the server must resend DATA
            out += packet[4:]
            expected += 1
        sock.sendto(struct.pack("!HH", 4, block), server)
        if block == expected - 1 and len(packet) - 4 < 512:
            sock.close()
            return bytes(out)


def test_tftp():
    for size in (1, 1000, 1024):
        data = os.urandom(size)
        server = bp.OneShotTftpServer("x.bin", data, bind="127.0.0.1", port=0,
                                      allowed_ip="127.0.0.1", timeout=0.3, verbose=False)
        server.start()
        got = tftp_fetch(server.port, "x.bin", drop_ack_for=2 if size > 512 else None)
        server.join(5)
        check(got == data, f"TFTP payload size {size}")
        check(server.error is None and server.done.is_set(), f"TFTP server error {server.error}")
        check(server.blocks == size // 512 + 1, f"TFTP block count {server.blocks}")
        if size > 512:
            check(server.retransmits >= 1, "TFTP retransmitted after a lost ACK")
    # A wrong name is refused with ERROR and the server keeps listening.
    server = bp.OneShotTftpServer("y.bin", b"z", bind="127.0.0.1", port=0, timeout=0.3,
                                  verbose=False)
    server.start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(5)
    sock.sendto(b"\0\x01other\0octet\0", ("127.0.0.1", server.port))
    packet, _ = sock.recvfrom(1024)
    check(packet[:4] == b"\0\x05\0\x01", "TFTP refuses unknown file")
    sock.close()
    check(tftp_fetch(server.port, "y.bin") == b"z", "TFTP serves after a refusal")
    server.join(5)


class Agent:
    def __init__(self, path):
        self.proc = subprocess.Popen([path, KEY.hex()], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reqs, outs = [], []
        while True:
            text = self.proc.stdout.readline().strip()
            check(text, "agent harness exited")
            if text == "END":
                return reqs, outs
            kind, rest = text.split(" ", 1)
            if kind == "REQ":
                reqs.append(rest.split(" "))
            elif kind == "OUT":
                port, payload = rest.split(" ")
                check(int(port) == CLIENT_PORT, "reply to client port")
                outs.append(bytes.fromhex(payload))

    def datagram(self, data, port=CLIENT_PORT):
        return self.send(f"{port} {data.hex()}")

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(5)


def handshake(agent, request_id):
    nonce = os.urandom(16)
    _, outs = agent.datagram(bp.build_hello(request_id, nonce))
    check(len(outs) == 1, "one CHALLENGE")
    parsed = bp.parse_challenge(KEY, outs[0], request_id, nonce)
    check(parsed is not None, "client authenticates the agent CHALLENGE")
    check(parsed[1] == 0x20260501 and parsed[2] == 10000, "challenge build id/window")
    check(bp.parse_challenge(bytes(32), outs[0], request_id, nonce) is None, "wrong key rejected")
    check(bp.parse_challenge(KEY, outs[0], request_id, bytes(16)) is None,
          "challenge bound to client nonce")
    return parsed[0], nonce


def test_interop(path):
    agent = Agent(path)
    try:
        server_nonce, client_nonce = handshake(agent, 11)
        sha = hashlib.sha256(b"payload").digest()
        cmd = bp.build_command(KEY, 11, server_nonce, bp.FILE, flags=bp.FLAG_PERSIST,
                               ip=bp.ip_to_int("192.168.50.208"),
                               mask=bp.ip_to_int("255.255.255.0"),
                               gateway=bp.ip_to_int("192.168.50.1"),
                               telemetry=bp.ip_to_int("192.168.50.17"), size=7, sha256=sha,
                               remote="book8-0001.bin", local="APP.ELF")
        reqs, outs = agent.datagram(cmd)
        check(len(reqs) == 1, f"agent accepted the client COMMAND: {reqs}")
        check(reqs[0] == ["2", "1", str(0xC0A832D0), str(0xFFFFFF00), str(0xC0A83201),
                          str(0xC0A83211), "7", sha.hex(), "book8-0001.bin", "APP.ELF"],
              f"agent decoded fields {reqs[0]}")
        check(len(outs) == 1, "RESULT sent")
        result = bp.parse_result(KEY, outs[0], 11, (server_nonce, client_nonce))
        check(result == {"command": bp.FILE, "status": bp.OK, "build_id": 0x20260501,
                         "message": "echo FILE"}, f"RESULT {result}")
        check(bp.parse_result(KEY, outs[0], 12, (server_nonce,)) is None, "request id bound")
        tampered = bytearray(outs[0])
        tampered[40] ^= 1
        check(bp.parse_result(KEY, bytes(tampered), 11, (server_nonce,)) is None, "RESULT MAC")
        # Client retransmission of the identical COMMAND gets the cached RESULT only.
        reqs, outs2 = agent.datagram(cmd)
        check(not reqs and outs2 == outs, "idempotent retransmit")
        # Spent nonce with a new request id is a replay: silently dropped.
        replay = bp.build_command(KEY, 99, server_nonce, bp.REBOOT)
        check(agent.datagram(replay) == ([], []), "replay dropped")

        # A stale CHALLENGE (earlier HELLO) must not be usable after a newer HELLO.
        old_nonce, _ = handshake(agent, 20)
        new_nonce, new_client = handshake(agent, 21)
        check(agent.datagram(bp.build_command(KEY, 20, old_nonce, bp.STATUS)) == ([], []),
              "stale nonce dropped")
        reqs, outs = agent.datagram(bp.build_command(KEY, 21, new_nonce, bp.STATUS))
        check(len(reqs) == 1 and reqs[0][0] == "0", "STATUS accepted with fresh nonce")
        check(bp.parse_result(KEY, outs[0], 21, (new_nonce, new_client))["message"]
              == "echo STATUS", "STATUS RESULT")

        for request_id, flags, local, accepted in (
                (24, 0, "", True), (25, 1, "", False), (26, 0, "OTHER.ELF", False)):
            nonce, client_nonce = handshake(agent, request_id)
            reqs, outs = agent.datagram(bp.build_command(
                KEY, request_id, nonce, bp.INSTALL_KERNEL, flags=flags,
                remote="new-kernel.bin", size=4096, local=local))
            result = bp.parse_result(KEY, outs[0], request_id, (nonce, client_nonce))
            check(bool(reqs) == accepted, "persistent command scope validation")
            check(result["status"] == (bp.OK if accepted else bp.BAD_REQUEST),
                  "persistent command authenticated result")
            if accepted:
                check(result["message"] == "echo INSTALL_KERNEL", "persistent command name")

        # Expired challenge window.
        nonce, _ = handshake(agent, 30)
        agent.send("tick 10001")
        check(agent.datagram(bp.build_command(KEY, 30, nonce, bp.STATUS)) == ([], []),
              "expired challenge dropped")
    finally:
        agent.close()


def test_client_over_udp(path):
    """Full PushClient.run() over real UDP sockets, bridged to the C agent."""
    import threading
    agent = Agent(path)
    bridge = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bridge.bind(("127.0.0.1", 0))
    bridge.settimeout(0.2)
    stop = threading.Event()
    seen = []

    def pump():
        while not stop.is_set():
            try:
                data, addr = bridge.recvfrom(2048)
            except socket.timeout:
                continue
            seen.append(data[5])
            if len(seen) == 1:
                continue        # drop the first HELLO, like a pending ARP lookup
            for out in agent.datagram(data)[1]:
                bridge.sendto(out, addr)

    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    try:
        client = bp.PushClient("127.0.0.1", KEY, bridge.getsockname()[1], verbose=False)
        result = client.run(bp.STATUS, result_timeout=5)
        check(result["status"] == bp.OK and result["message"] == "echo STATUS",
              f"PushClient STATUS {result}")
        check(seen[:2] == [bp.HELLO, bp.HELLO], "client retried HELLO")
        check(client.last_challenge == {"build_id": 0x20260501, "window_ms": 10000},
              "challenge info")
        bad = bp.PushClient("127.0.0.1", bytes(32), bridge.getsockname()[1], verbose=False)
        try:
            bad.run(bp.STATUS, hello_attempts=2)
        except bp.PushError as err:
            check("CHALLENGE" in str(err), f"wrong-key error {err}")
        else:
            check(False, "wrong key accepted")
    finally:
        stop.set()
        thread.join(2)
        bridge.close()
        agent.close()


def main():
    test_layout()
    test_kernel_check()
    test_tftp()
    harness = sys.argv[1] if len(sys.argv) > 1 else None
    if harness:
        test_interop(harness)
        test_client_over_udp(harness)
        print("PASS: push client layout, kernel precheck, TFTP server, C agent interop")
    else:
        print("PASS: push client layout, kernel precheck, TFTP server (no interop harness)")


if __name__ == "__main__":
    main()
