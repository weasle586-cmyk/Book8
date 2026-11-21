#!/usr/bin/env python3
"""Host test: telemetry_listener.py / telemetry_profile.py decode v1 and v2 packets.

Packets are assembled at explicit byte offsets (net/net.h layout) rather than with
the listener's own format strings, so a format drift is caught.
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import telemetry_listener as tl  # noqa: E402
import telemetry_profile as tp  # noqa: E402


def common(magic, seq, uptime_ms, size):
    p = bytearray(size)
    struct.pack_into(">IIQ", p, 0, magic, seq, uptime_ms)
    struct.pack_into(">II", p, 16, 0x7, 3200)              # cpu flags, MHz
    struct.pack_into(">QQQQ", p, 24, 4 << 30, 16 << 20, 1 << 20, 8 << 20)
    struct.pack_into(">IIIII", p, 56, 10 + seq, 20 + seq, 1, 2, 3)
    return p


def v1_packet(seq, uptime_ms, log=b"boot ok"):
    p = common(0x424B3801, seq, uptime_ms, 204)
    p[76:76 + len(log)] = log
    return bytes(p)


def v2_packet(seq, uptime_ms, log=b"rtl8168 link up"):
    p = common(0x424B3802, seq, uptime_ms, 292)
    ext = list(range(100, 122))
    ext[0] = 1767225600                   # build_id
    ext[1] = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 11)  # health
    ext[2] = 1                            # relaunch_count
    ext[3] = 0x02 | 0x01 | 0x10           # LINK FDX 1000M
    struct.pack_into(">22I", p, 76, *ext)
    p[164:164 + len(log)] = log
    return bytes(p)


def check(cond, message):
    if not cond:
        print(f"FAIL: {message}")
        sys.exit(1)


def main():
    if len(sys.argv) == 2:
        wire = subprocess.run([sys.argv[1], "--wire"], check=True, stdout=subprocess.PIPE)
        observation = tl.decode_packet(wire.stdout)
        check(len(wire.stdout) == 292, "C helper emits unchanged v2 packet size")
        check("mapping" not in observation, "disabled snapshot is not serialized")
        check(observation["log"] == "local diagnostics disabled", "ordinary event preserved")
        check(observation["cpu_mhz"] == 3200 and observation["build_id"] == 12345,
              "mapping preserves existing CPU and extended diagnostics")
        print("PASS: C helper -> v2 UDP 8080 serialization without NVMAP snapshot")
    m1 = tl.decode_packet(v1_packet(5, 1500))
    check(m1["version"] == 1 and m1["seq"] == 5 and m1["timestamp_ms"] == 1500, "v1 header")
    check(m1["cpu_mhz"] == 3200 and m1["mem_total"] == 4 << 30, "v1 metrics")
    check(m1["tx_packets"] == 15 and m1["tx_timeouts"] == 3 and m1["log"] == "boot ok", "v1 tail")
    check("build_id" not in m1, "v1 has no extended fields")

    m2 = tl.decode_packet(v2_packet(6, 2500))
    check(m2["version"] == 2 and m2["seq"] == 6 and m2["heap_total"] == 8 << 20, "v2 common")
    check(m2["build_id"] == 1767225600 and m2["relaunch_count"] == 1, "v2 build/relaunch")
    check(m2["phy_status"] == 0x13 and m2["tx_ring_pending"] == 104, "v2 phy/rings")
    check(m2["push_rejected"] == 121 and m2["poll_max_us"] == 118, "v2 last extended")
    check(m2["log"] == "rtl8168 link up", "v2 log")
    check(tl.format_bits(m2["phy_status"], tl.PHY_BITS) == "FDX|LINK|1000M", "phy bits")
    check(tl.format_bits(m2["health_flags"], tl.HEALTH_FLAGS)
          == "IPV4|CFG_BAKED|PUSH|LINK|PCI_SOUND", "health bits")
    line = tl.format_entry("now", "192.168.50.208", m2)
    check("v2" in line and "build=1767225600" in line and "phy=0x13(FDX|LINK|1000M)" in line,
          f"format_entry v2: {line}")
    check("v1" in tl.format_entry("now", "x", m1), "format_entry v1")

    nvlog = (b"NVMAP1 3 13c210de 0218 fe00000000000000 0000000001000000 "
             b"00000000d0000000 0000000010000000 124000a1 12345678 0")
    nv = tl.decode_packet(v2_packet(7, 3000, nvlog))
    check(nv["mapping"]["bar0_phys"] == 0xfe00000000000000, "nvmap full uint64")
    check(nv["mapping"]["pmc_enable"] == 0x12345678, "nvmap register")
    line = tl.format_entry("now", "x", nv)
    check("power_gating=unknown" in line and "13C210DE@02:03.0" in line, "nvmap display")
    check("mapping" not in tl.decode_packet(v1_packet(1, 1, nvlog)), "v1 log unchanged")
    deferred = nvlog.replace(b"12345678 0", b"00000000 b")
    nv = tl.decode_packet(v2_packet(8, 4000, deferred))
    check("unavailable(errno=11)" in tl.format_entry("now", "x", nv), "nvmap deferred")
    for bad in (b"NVMAP1", nvlog + b" 0", nvlog.replace(b"NVMAP1 3", b"NVMAP1 7"),
                nvlog.replace(b"12345678", b"1234567z"), nvlog[:-1] + b"100",
                nvlog[:-1] + b"b"):
        try:
            tl.decode_packet(v2_packet(1, 1, bad))
        except ValueError:
            pass
        else:
            check(False, f"accepted bad mapping log {bad!r}")

    for bad, reason in ((v2_packet(1, 1)[:-1], "v2 size"), (v1_packet(1, 1) + b"\0", "size"),
                        (b"\x01\x38\x4b\x42" + bytes(200), "magic")):
        try:
            tl.decode_packet(bad)
        except ValueError as err:
            check(reason in str(err), f"rejection reason {err}")
        else:
            check(False, f"accepted bad packet ({reason})")

    records = [(1000.0 + i, "192.168.50.208", 4000, v2_packet(10 + i, 1000 * (i + 1)))
               for i in range(5)]
    records.insert(2, (1001.5, "192.168.50.208", 4000, b"junk"))
    stream, samples = tp.analyze_stream(records)
    check(stream["valid"] == 5 and stream["sequence"]["lost"] == 0, "stream analysis")
    profile = tp.build_profile(stream, samples, None, "test")
    check("diagnostics_v2" in profile, "profile has v2 diagnostics")
    text = tp.render(profile)
    check("V2 DIAGNOSTICS" in text and "1767225600" in text, "rendered v2 section")
    print("PASS: telemetry v1/v2 decode, format and profile")


if __name__ == "__main__":
    main()
