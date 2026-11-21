"""BOOK8 UDP telemetry listener.

Wire format must match net/net.h (#pragma pack(1)). Every integer is
serialized in network byte order (big-endian) by the kernel. Two versions are
accepted, distinguished by magic and size:

v1  magic 0x424B3801 ('BK8\\x01'), 204 bytes (struct net_telemetry_packet)
v2  magic 0x424B3802 ('BK8\\x02'), 292 bytes (struct net_telemetry_packet_v2)

  off  size  field                  (v1 and v2)
    0     4  magic
    4     4  sequence
    8     8  timestamp_ms          kernel uptime in milliseconds
   16     4  cpu_flags             bit0 desktop, bit1 IRQs enabled, bit2 NIC running
   20     4  cpu_mhz
   24     8  memory_total_bytes
   32     8  kernel_reserved_bytes
   40     8  heap_used_bytes
   48     8  heap_total_bytes
   56     4  tx_packets
   60     4  rx_packets
   64     4  tx_dropped
   68     4  rx_dropped
   72     4  tx_timeouts
  v1:  76  128  log                NUL-terminated ASCII          -> 204 bytes
  v2:  76   88  extended diagnostics (22 x u32, see EXTENDED_FIELDS)
      164  128  log                                              -> 292 bytes

NVMAP1 diagnostic snapshots use the existing v2 log field, not new wire fields.
They report mapping metadata and a userspace register observation; internal
NVIDIA power-gating state is not measured.
"""

import argparse
import datetime
import json
import socket
import struct
import time

TELEMETRY_PORT = 8080
MAGIC_EXPECTED = 0x424B3801
MAGIC_V2 = 0x424B3802
PACKET_FORMAT = "!IIQIIQQQQIIIII128s"
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)
assert PACKET_SIZE == 204, PACKET_SIZE

# net_telemetry_extended, in wire order.
EXTENDED_FIELDS = (
    "build_id", "health_flags", "relaunch_count", "phy_status",
    "tx_ring_pending", "rx_ring_ready",
    "net_rx_frames", "net_tx_frames", "net_invalid_frames", "net_ignored_frames",
    "net_rx_errors", "net_tx_errors", "arp_replies", "echo_replies", "udp_received",
    "arp_entries", "poll_iterations", "poll_avg_us", "poll_max_us",
    "telemetry_failures", "push_accepted", "push_rejected",
)
PACKET_FORMAT_V2 = "!IIQIIQQQQIIIII" + "I" * len(EXTENDED_FIELDS) + "128s"
PACKET_SIZE_V2 = struct.calcsize(PACKET_FORMAT_V2)
assert PACKET_SIZE_V2 == 292, PACKET_SIZE_V2

CPU_DESKTOP = 1
CPU_IRQ_ENABLED = 2
CPU_NIC_RUNNING = 4

# BOOK8_HEALTH_* in net/net.h.
HEALTH_FLAGS = (
    (1 << 0, "IPV4"), (1 << 1, "CFG_BAKED"), (1 << 2, "CFG_FILE"), (1 << 3, "PUSH"),
    (1 << 4, "LINK"), (1 << 5, "STORAGE"), (1 << 6, "USB_KBD"), (1 << 7, "USB_MOUSE"),
    (1 << 8, "PS2_KBD"), (1 << 9, "PS2_MOUSE"), (1 << 10, "FB"), (1 << 11, "PCI_SOUND"),
    (1 << 12, "ACPI_S5"), (1 << 13, "RELAUNCHED"),
)
HEALTH_PHY_LINK = 1 << 4

# RTL8168 PHYstatus register (0x6C).
PHY_BITS = ((0x01, "FDX"), (0x02, "LINK"), (0x04, "10M"), (0x08, "100M"),
            (0x10, "1000M"), (0x20, "RXFLOW"), (0x40, "TXFLOW"))

COMMON_FIELDS = ("magic", "seq", "timestamp_ms", "cpu_flags", "cpu_mhz", "mem_total",
                 "kernel_reserved", "heap_used", "heap_total", "tx_packets", "rx_packets",
                 "tx_dropped", "rx_dropped", "tx_timeouts")

MAPPING_FIELDS = (
    "flags", "pci_id", "bdf", "bar0_phys", "bar0_size", "bar1_phys", "bar1_size",
    "boot0", "pmc_enable", "read_errno",
)
MAPPING_WIDTHS = (1, 8, 4, 16, 16, 16, 16, 8, 8, 2)


def decode_mapping(log):
    """Decode the versioned ASCII envelope carried at v2 offset 164."""
    parts = log.split(" ")
    if len(parts) != len(MAPPING_FIELDS) + 1 or parts[0] != "NVMAP1":
        raise ValueError("invalid NVMAP1 field count")
    for text, width in zip(parts[1:], MAPPING_WIDTHS):
        if not 1 <= len(text) <= width or any(c not in "0123456789abcdef" for c in text):
            raise ValueError("invalid NVMAP1 hexadecimal field")
    mapping = dict(zip(MAPPING_FIELDS, (int(text, 16) for text in parts[1:])))
    if mapping["flags"] not in (0, 1, 3) or mapping["read_errno"] not in (0, 1, 5, 11, 19):
        raise ValueError("invalid NVMAP1 flags or errno")
    if mapping["read_errno"] and mapping["pmc_enable"]:
        raise ValueError("invalid NVMAP1 unavailable register value")
    return mapping


def decode_packet(data):
    """Return a dict for a valid BOOK8 v1 or v2 telemetry packet, else raise ValueError.

    The dict always has the v1 keys plus "version"; v2 adds the EXTENDED_FIELDS keys.
    """
    if len(data) < 4:
        raise ValueError(f"size {len(data)} is too short")
    magic = struct.unpack_from("!I", data)[0]
    if magic == MAGIC_V2:
        if len(data) != PACKET_SIZE_V2:
            raise ValueError(f"v2 size {len(data)} != {PACKET_SIZE_V2}")
        values = struct.unpack(PACKET_FORMAT_V2, data)
        version = 2
    elif magic == MAGIC_EXPECTED:
        if len(data) != PACKET_SIZE:
            raise ValueError(f"size {len(data)} != {PACKET_SIZE}")
        values = struct.unpack(PACKET_FORMAT, data)
        version = 1
    else:
        raise ValueError(f"magic 0x{magic:08X} != 0x{MAGIC_EXPECTED:08X}/0x{MAGIC_V2:08X}")
    common = len(COMMON_FIELDS)
    m = dict(zip(COMMON_FIELDS, values[:common]))
    del m["magic"]
    m["version"] = version
    if version == 2:
        m.update(zip(EXTENDED_FIELDS, values[common:common + len(EXTENDED_FIELDS)]))
    m["log"] = values[-1].split(b"\0", 1)[0].decode("ascii", "replace")
    if version == 2 and (m["log"] == "NVMAP1" or m["log"].startswith("NVMAP1 ")):
        m["mapping"] = decode_mapping(m["log"])
    return m


def format_bits(value, table):
    names = [name for bit, name in table if value & bit]
    return "|".join(names) if names else "-"


def format_flags(flags):
    names = []
    if flags & CPU_DESKTOP:
        names.append("DESKTOP")
    if flags & CPU_IRQ_ENABLED:
        names.append("IRQ")
    if flags & CPU_NIC_RUNNING:
        names.append("NIC")
    return "|".join(names) if names else "-"


def mib(value):
    return value / (1024 * 1024)


def format_entry(now, src, m):
    heap_pct = (m["heap_used"] * 100.0 / m["heap_total"]) if m["heap_total"] else 0.0
    line = (f"[{now}] {src} v{m.get('version', 1)} seq={m['seq']} "
            f"up={m['timestamp_ms'] / 1000:.3f}s "
            f"cpu=0x{m['cpu_flags']:X}({format_flags(m['cpu_flags'])}) {m['cpu_mhz']}MHz | "
            f"mem={mib(m['mem_total']):.1f}MiB kernel={mib(m['kernel_reserved']):.1f}MiB "
            f"heap={m['heap_used']}/{m['heap_total']}B ({heap_pct:.1f}%) | "
            f"tx={m['tx_packets']} rx={m['rx_packets']} tx_drop={m['tx_dropped']} "
            f"rx_drop={m['rx_dropped']} tx_timeout={m['tx_timeouts']}")
    if m.get("version") == 2:
        line += (f" | build={m['build_id']} relaunch={m['relaunch_count']} "
                 f"health=0x{m['health_flags']:04X}({format_bits(m['health_flags'], HEALTH_FLAGS)}) "
                 f"phy=0x{m['phy_status']:02X}({format_bits(m['phy_status'], PHY_BITS)}) | "
                 f"rings tx_pending={m['tx_ring_pending']} rx_ready={m['rx_ring_ready']} | "
                 f"net rx={m['net_rx_frames']} tx={m['net_tx_frames']} "
                 f"inval={m['net_invalid_frames']} ign={m['net_ignored_frames']} "
                 f"rx_err={m['net_rx_errors']} tx_err={m['net_tx_errors']} "
                 f"arp={m['arp_replies']}/{m['arp_entries']}cached echo={m['echo_replies']} "
                 f"udp={m['udp_received']} | poll n={m['poll_iterations']} "
                 f"avg={m['poll_avg_us']}us max={m['poll_max_us']}us | "
                 f"tele_fail={m['telemetry_failures']} "
                 f"push ok/rej={m['push_accepted']}/{m['push_rejected']}")
    if "mapping" in m:
        nv = m["mapping"]
        bdf = nv["bdf"]
        register = (f"0x{nv['pmc_enable']:08X}" if nv["read_errno"] == 0
                    else f"unavailable(errno={nv['read_errno']})")
        line += (f" | nvmap present={bool(nv['flags'] & 1)} mapped={bool(nv['flags'] & 2)} "
                 f"pci=0x{nv['pci_id']:08X}@{bdf >> 8:02X}:{(bdf >> 3) & 31:02X}.{bdf & 7} "
                 f"BAR0=0x{nv['bar0_phys']:X}+0x{nv['bar0_size']:X} "
                 f"BAR1=0x{nv['bar1_phys']:X}+0x{nv['bar1_size']:X} "
                 f"boot0_cached=0x{nv['boot0']:08X} pmc_enable={register} "
                 "power_gating=unknown (userspace snapshot, not live)")
    return line + f" | {m['log']!r}"


def main():
    parser = argparse.ArgumentParser(description="BOOK8 UDP telemetry listener")
    parser.add_argument("--bind", default="0.0.0.0", help="local address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=TELEMETRY_PORT)
    parser.add_argument("--source", default=None,
                        help="only accept packets from this IP (e.g. 192.168.50.208)")
    parser.add_argument("--log", default="book8_telemetry.log")
    parser.add_argument("--raw-out", default=None,
                        help="also append every datagram as JSON lines for telemetry_profile.py")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)  # lets Ctrl+C interrupt recvfrom on Windows
    print(f"[*] BOOK8 telemetry listener on UDP {args.bind}:{args.port} "
          f"(v1 {PACKET_SIZE}-byte / v2 {PACKET_SIZE_V2}-byte big-endian packets)")

    last_seq = {}
    last_build = {}
    raw_file = open(args.raw_out, "a", encoding="utf-8") if args.raw_out else None
    with open(args.log, "a", encoding="utf-8") as log_file:
        try:
            while True:
                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                if args.source and addr[0] != args.source:
                    continue
                src = f"{addr[0]}:{addr[1]}"
                if raw_file:
                    raw_file.write(json.dumps({"t": time.time(), "src": addr[0],
                                               "port": addr[1], "hex": data.hex()}) + "\n")
                    raw_file.flush()
                try:
                    m = decode_packet(data)
                except ValueError as err:
                    print(f"[!] Dropped packet from {src}: {err}; first bytes: {data[:24].hex(' ')}")
                    continue

                prev = last_seq.get(addr[0])
                if prev is not None:
                    expected = (prev + 1) & 0xFFFFFFFF
                    if m["seq"] != expected:
                        gap = (m["seq"] - expected) & 0xFFFFFFFF
                        note = f"[!] Sequence gap from {src}: expected {expected}, got {m['seq']}"
                        if gap < 0x80000000:
                            note += f" ({gap} lost)"
                        else:
                            note += " (sender restarted or reordered)"
                        print(note)
                        log_file.write(note + "\n")
                last_seq[addr[0]] = m["seq"]

                if m["version"] == 2:
                    identity = (m["build_id"], m["relaunch_count"])
                    before = last_build.get(addr[0])
                    if before is not None and before != identity:
                        note = (f"[*] {addr[0]} now runs build {identity[0]} "
                                f"(relaunch #{identity[1]}); was build {before[0]} "
                                f"(relaunch #{before[1]})")
                        print(note)
                        log_file.write(note + "\n")
                    last_build[addr[0]] = identity

                now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                entry = format_entry(now, src, m)
                print(entry)
                log_file.write(entry + "\n")
                log_file.flush()
        except KeyboardInterrupt:
            print("\n[*] Telemetry listener shut down.")
        finally:
            sock.close()
            if raw_file:
                raw_file.close()


if __name__ == "__main__":
    main()
