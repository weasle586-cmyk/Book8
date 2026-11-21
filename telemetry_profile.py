"""BOOK8 hardware/telemetry profile generator.

Builds a structured hardware profile from:
  * the UDP telemetry stream (204-byte v1 or 292-byte v2; live capture or a
    --raw-out capture file written by telemetry_listener.py), and
  * optional BOOK8 serial/console logs (COM1 capture or pasted NETSTAT output),
    which are the only source of NIC PCI/BAR/MAC/init and ACPI details.

Anything BOOK8 does not measure is reported explicitly as "not reported";
values are never synthesized.

Examples:
  python telemetry_profile.py --live 60 --source 192.168.50.208
  python telemetry_profile.py --raw capture.jsonl --serial-log com1.log --json profile.json
"""

import argparse
import datetime
import json
import re
import socket
import statistics
import time

from telemetry_listener import (CPU_DESKTOP, CPU_IRQ_ENABLED, CPU_NIC_RUNNING, HEALTH_FLAGS,
                                PACKET_SIZE, PACKET_SIZE_V2, PHY_BITS, TELEMETRY_PORT,
                                decode_packet, format_bits)

U32 = 1 << 32
NOT_REPORTED = "not reported by BOOK8"
COUNTERS = ("tx_packets", "rx_packets", "tx_dropped", "rx_dropped", "tx_timeouts")
AUDIO_RE = re.compile(r"audio|sound|\bhda\b|azalia|ac'?97|codec|\bpcm\b|speaker", re.I)


# --------------------------------------------------------------------------- input

def capture_live(bind, port, seconds, source):
    """Return [(host_time, src_ip, src_port, bytes)] received within `seconds`."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))
    sock.settimeout(0.5)
    records = []
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            if source and addr[0] != source:
                continue
            records.append((time.time(), addr[0], addr[1], data))
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
    return records


def load_raw(path, source):
    records = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if source and r["src"] != source:
                continue
            records.append((r["t"], r["src"], r["port"], bytes.fromhex(r["hex"])))
    return records


# --------------------------------------------------------------------------- serial logs

SERIAL_PATTERNS = {
    "pci": re.compile(r"BOOK8-RTL8168: PCI (\d+):(\d+)\.(\d+) vendor/device=([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})"),
    "bar": re.compile(r"BOOK8-RTL8168: BAR(\d) (I/O|MMIO)=0x([0-9A-Fa-f]{16}) size=0x([0-9A-Fa-f]{16})"),
    "mac": re.compile(r"BOOK8-RTL8168: MAC=((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})"),
    "rtl": re.compile(r"BOOK8-RTL8168: (.+)"),
    "net": re.compile(r"BOOK8-NET: (.+)"),
    "ipcfg": re.compile(r"IP=(\S+) MASK=(\S+) GATEWAY=(\S+)"),
    "netstat": re.compile(r"RX/TX/invalid/ignored/RX-errors/TX-errors/ARP/echo/UDP=([\d/]+)"),
    "tele": re.compile(r"TELEMETRY target=(\S+) UDP=(\d+) interval=(\d+)ms"),
    "acpi": re.compile(r"ACPI: (.+)"),
    "fb": re.compile(r"BOOK8 framebuffer handoff: (.+)"),
}
NETSTAT_FIELDS = ("rx_frames", "tx_frames", "invalid_frames", "ignored_frames",
                  "rx_errors", "tx_errors", "arp_replies", "echo_replies", "udp_received")


def parse_serial(paths):
    s = {"files": list(paths), "pci": None, "bars": {}, "mac": None, "rtl_status": [],
         "net_events": [], "ip_config": None, "netstat": None, "telemetry_cfg": None,
         "acpi": [], "framebuffer": None, "audio_lines": []}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                if AUDIO_RE.search(line):
                    s["audio_lines"].append(line.strip())
                m = SERIAL_PATTERNS["pci"].search(line)
                if m:
                    s["pci"] = {"bdf": f"{int(m[1]):02X}:{int(m[2]):02X}.{m[3]}",
                                "vendor": f"0x{m[4].upper()}", "device": f"0x{m[5].upper()}"}
                    continue
                m = SERIAL_PATTERNS["bar"].search(line)
                if m:
                    base, size = int(m[3], 16), int(m[4], 16)
                    s["bars"][int(m[1])] = {"type": m[2], "base": f"0x{base:X}",
                                            "end": f"0x{base + size - 1:X}", "size": f"0x{size:X}"}
                    continue
                m = SERIAL_PATTERNS["mac"].search(line)
                if m:
                    s["mac"] = m[1].upper()
                    continue
                m = SERIAL_PATTERNS["rtl"].search(line)
                if m:
                    s["rtl_status"].append(m[1].strip())
                    continue
                m = SERIAL_PATTERNS["net"].search(line)
                if m:
                    s["net_events"].append(m[1].strip())
                    continue
                m = SERIAL_PATTERNS["ipcfg"].search(line)
                if m:
                    s["ip_config"] = {"ip": m[1], "mask": m[2], "gateway": m[3]}
                m = SERIAL_PATTERNS["netstat"].search(line)
                if m:
                    vals = [int(v) for v in m[1].split("/")]
                    if len(vals) == len(NETSTAT_FIELDS):
                        s["netstat"] = dict(zip(NETSTAT_FIELDS, vals))
                    continue
                m = SERIAL_PATTERNS["tele"].search(line)
                if m:
                    s["telemetry_cfg"] = {"target": m[1], "port": int(m[2]), "interval_ms": int(m[3])}
                    continue
                m = SERIAL_PATTERNS["acpi"].search(line)
                if m:
                    s["acpi"].append(m[1].strip())
                    continue
                m = SERIAL_PATTERNS["fb"].search(line)
                if m:
                    s["framebuffer"] = m[1].strip()
    s["bars"] = {f"BAR{k}": v for k, v in sorted(s["bars"].items())}
    return s


# --------------------------------------------------------------------------- analysis

def summarize(values):
    if not values:
        return None
    return {"min": min(values), "max": max(values), "mean": round(statistics.fmean(values), 3),
            "last": values[-1]}


def slope_per_s(times_ms, values):
    """Least-squares slope in units per second, or None."""
    if len(values) < 2 or times_ms[-1] == times_ms[0]:
        return None
    t = [x / 1000.0 for x in times_ms]
    mt, mv = statistics.fmean(t), statistics.fmean(values)
    den = sum((x - mt) ** 2 for x in t)
    return None if den == 0 else sum((x - mt) * (v - mv) for x, v in zip(t, values)) / den


def analyze_stream(records):
    out = {"datagrams": len(records), "valid": 0, "rejected": {}, "sources": {}}
    samples = []
    for host_t, ip, port, data in records:
        out["sources"][f"{ip}:{port}"] = out["sources"].get(f"{ip}:{port}", 0) + 1
        try:
            m = decode_packet(data)
        except ValueError as err:
            reason = str(err).split(" !=")[0]
            out["rejected"][reason] = out["rejected"].get(reason, 0) + 1
            continue
        m["host_t"] = host_t
        samples.append(m)
    out["valid"] = len(samples)
    if not samples:
        return out, samples

    # Split into boot sessions: uptime going backwards means BOOK8 rebooted.
    sessions, cur = [], [samples[0]]
    for prev, m in zip(samples, samples[1:]):
        if m["timestamp_ms"] < prev["timestamp_ms"]:
            sessions.append(cur)
            cur = []
        cur.append(m)
    sessions.append(cur)
    out["boot_sessions"] = len(sessions)

    lost = dup = reorder = 0
    intervals = []
    deltas = {c: 0 for c in COUNTERS}
    elapsed_ms = 0
    for sess in sessions:
        for prev, m in zip(sess, sess[1:]):
            gap = (m["seq"] - prev["seq"]) % U32
            if gap == 0:
                dup += 1
                continue
            if gap >= 0x80000000:
                reorder += 1
                continue
            lost += gap - 1
            intervals.append(m["timestamp_ms"] - prev["timestamp_ms"])
            for c in COUNTERS:
                deltas[c] += (m[c] - prev[c]) % U32
        elapsed_ms += sess[-1]["timestamp_ms"] - sess[0]["timestamp_ms"]
    expected = out["valid"] + lost
    out["sequence"] = {"first": samples[0]["seq"], "last": samples[-1]["seq"], "lost": lost,
                       "duplicates": dup, "reordered": reorder,
                       "loss_pct": round(100.0 * lost / expected, 3) if expected else 0.0}
    out["interval_ms"] = summarize(intervals)
    if intervals and len(intervals) > 1:
        out["interval_ms"]["stdev"] = round(statistics.stdev(intervals), 3)
    out["uptime_ms"] = {"first": samples[0]["timestamp_ms"], "last": samples[-1]["timestamp_ms"],
                        "observed_span_s": round(elapsed_ms / 1000.0, 3)}
    out["host_span_s"] = round(samples[-1]["host_t"] - samples[0]["host_t"], 3)
    out["_deltas"], out["_elapsed_ms"] = deltas, elapsed_ms
    return out, samples


def rate(delta, elapsed_ms):
    return round(delta * 1000.0 / elapsed_ms, 3) if elapsed_ms > 0 else None


def build_profile(stream, samples, serial, source_desc):
    last = samples[-1] if samples else None
    deltas, elapsed = stream.pop("_deltas", None), stream.pop("_elapsed_ms", 0)
    n = len(samples)

    def flag_pct(bit):
        return round(100.0 * sum(1 for s in samples if s["cpu_flags"] & bit) / n, 1) if n else None

    # ---- network
    nic = {
        "pci": serial["pci"] if serial else None,
        "bars": serial["bars"] if serial else {},
        "mac": serial["mac"] if serial else None,
        "driver_status": serial["rtl_status"] if serial else [],
        "ip_config": serial["ip_config"] if serial else None,
        "link_state": {
            "phy_register": NOT_REPORTED + " (driver does not read PHYStatus 0x6C)",
            "nic_running_flag_pct": flag_pct(CPU_NIC_RUNNING),
            "inferred": None,
        },
        "telemetry_counters": None,
        "netstat_snapshot": serial["netstat"] if serial else None,
        "net_events": serial["net_events"] if serial else [],
    }
    if last:
        tx_attempts = deltas["tx_packets"] + deltas["tx_dropped"] + deltas["tx_timeouts"]
        rx_attempts = deltas["rx_packets"] + deltas["rx_dropped"]
        nic["telemetry_counters"] = {
            "last_absolute": {c: last[c] for c in COUNTERS},
            "delta_over_capture": deltas,
            "rate_per_s": {c: rate(deltas[c], elapsed) for c in COUNTERS},
            "tx_drop_pct": round(100.0 * deltas["tx_dropped"] / tx_attempts, 3) if tx_attempts else 0.0,
            "tx_timeout_pct": round(100.0 * deltas["tx_timeouts"] / tx_attempts, 3) if tx_attempts else 0.0,
            "rx_drop_pct": round(100.0 * deltas["rx_dropped"] / rx_attempts, 3) if rx_attempts else 0.0,
            "note": "TX counters include BOOK8's own telemetry datagrams (~1/s).",
        }
        # Telemetry packets arriving here prove the TX path and physical link work.
        nic["link_state"]["inferred"] = (
            "up (telemetry frames received from the NIC"
            + (", RX counter advancing" if deltas["rx_packets"] else "; no RX traffic observed")
            + ")")

    # ---- audio
    log_msgs = {}
    for s in samples:
        log_msgs[s["log"]] = log_msgs.get(s["log"], 0) + 1
    audio_stream_logs = sorted(m for m in log_msgs if AUDIO_RE.search(m))
    audio = {
        "driver": "none - BOOK8 has no audio driver; PCI class 0x04 is only detected for the "
                  "Control Panel 'Sound / Audio' page",
        "hardware_identification": NOT_REPORTED,
        "interrupt_configuration": NOT_REPORTED,
        "buffer_allocation": NOT_REPORTED,
        "telemetry_log_entries": audio_stream_logs,
        "serial_log_entries": serial["audio_lines"] if serial else [],
    }

    # ---- CPU / chipset
    mhz = [s["cpu_mhz"] for s in samples]
    cpu = {
        "cpu_mhz": summarize(mhz),
        "cpu_mhz_note": "TSC-calibrated at boot; constant by design, not a live P-state clock",
        "flags_last": None if not last else {
            "raw": f"0x{last['cpu_flags']:X}",
            "desktop_active": bool(last["cpu_flags"] & CPU_DESKTOP),
            "interrupts_enabled": bool(last["cpu_flags"] & CPU_IRQ_ENABLED),
            "nic_running": bool(last["cpu_flags"] & CPU_NIC_RUNNING),
        },
        "flags_pct_of_samples": {"desktop_active": flag_pct(CPU_DESKTOP),
                                 "interrupts_enabled": flag_pct(CPU_IRQ_ENABLED),
                                 "nic_running": flag_pct(CPU_NIC_RUNNING)},
        "unknown_flag_bits_seen": sorted({f"0x{s['cpu_flags'] & ~7:X}" for s in samples
                                          if s["cpu_flags"] & ~7}),
        "core_states": NOT_REPORTED + " (single-core kernel; no C-state/MPERF/APERF sampling)",
        "power_management_registers": NOT_REPORTED + " in telemetry",
        "acpi_from_serial": serial["acpi"] if serial else [],
        "framebuffer_from_serial": serial["framebuffer"] if serial else None,
    }

    # ---- memory
    memory = None
    if last:
        current = [last]
        for s in reversed(samples[:-1]):
            if s["timestamp_ms"] > current[0]["timestamp_ms"]:
                break
            current.insert(0, s)
        t = [s["timestamp_ms"] for s in current]
        used = [s["heap_used"] for s in samples]
        growth = slope_per_s(t, [s["heap_used"] for s in current])
        memory = {
            "total_bytes": last["mem_total"],
            "total_mib": round(last["mem_total"] / 2**20, 1),
            "kernel_reserved_bytes": last["kernel_reserved"],
            "kernel_reserved_mib": round(last["kernel_reserved"] / 2**20, 3),
            "heap_total_bytes": last["heap_total"],
            "heap_used_bytes": summarize(used),
            "heap_used_pct_last": round(100.0 * last["heap_used"] / last["heap_total"], 3)
            if last["heap_total"] else None,
            "heap_growth_bytes_per_s_current_boot": None if growth is None else round(growth, 3),
            "available_outside_kernel_bytes": max(0, last["mem_total"] - last["kernel_reserved"]),
            "values_changed_during_capture": sorted(
                k for k in ("mem_total", "kernel_reserved", "heap_total")
                if len({s[k] for s in samples}) > 1),
            "note": "Heap lives inside the kernel-reserved extent.",
        }

    if last and last.get("version") == 2:
        phy = last["phy_status"]
        nic["link_state"]["phy_register"] = (
            f"0x{phy:02X} ({format_bits(phy, PHY_BITS)}) from RTL8168 PHYstatus 0x6C")
        nic["link_state"]["inferred"] = "up" if phy & 0x02 else "DOWN per PHYstatus"

    return {
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "input": source_desc,
        "stream": stream,
        "network": nic,
        "audio": audio,
        "cpu_chipset": cpu,
        "memory": memory,
        "diagnostics_v2": build_v2(samples),
        "log_messages": log_msgs,
    }


def build_v2(samples):
    """Aggregates the v2 extended block; None when only v1 packets were seen."""
    v2 = [s for s in samples if s.get("version") == 2]
    if not v2:
        return None
    last = v2[-1]
    builds = []
    for s in v2:
        identity = {"build_id": s["build_id"], "relaunch_count": s["relaunch_count"]}
        if not builds or builds[-1] != identity:
            builds.append(identity)
    health_pct = {name: round(100.0 * sum(1 for s in v2 if s["health_flags"] & bit) / len(v2), 1)
                  for bit, name in HEALTH_FLAGS}
    return {
        "samples": len(v2),
        "build_id": last["build_id"],
        "relaunch_count": last["relaunch_count"],
        "build_history": builds,
        "health_flags_last": f"0x{last['health_flags']:04X} "
                             f"({format_bits(last['health_flags'], HEALTH_FLAGS)})",
        "health_flags_pct_of_samples": health_pct,
        "phy_status_values": sorted({f"0x{s['phy_status']:02X} ({format_bits(s['phy_status'], PHY_BITS)})"
                                     for s in v2}),
        "link_down_samples": sum(1 for s in v2 if not s["phy_status"] & 0x02),
        "tx_ring_pending": summarize([s["tx_ring_pending"] for s in v2]),
        "rx_ring_ready": summarize([s["rx_ring_ready"] for s in v2]),
        "poll_iterations_per_sample": summarize([s["poll_iterations"] for s in v2]),
        "poll_avg_us": summarize([s["poll_avg_us"] for s in v2]),
        "poll_max_us": summarize([s["poll_max_us"] for s in v2]),
        "stack_counters_last": {k: last[k] for k in (
            "net_rx_frames", "net_tx_frames", "net_invalid_frames", "net_ignored_frames",
            "net_rx_errors", "net_tx_errors", "arp_replies", "echo_replies", "udp_received",
            "arp_entries")},
        "telemetry_failures_last": last["telemetry_failures"],
        "push_accepted_last": last["push_accepted"],
        "push_rejected_last": last["push_rejected"],
    }


# --------------------------------------------------------------------------- report

def render(p):
    L = []
    add = L.append

    def kv(k, v, ind=2):
        add(f"{' ' * ind}{k:<30} {v}")

    add("=" * 78)
    add(f"BOOK8 HARDWARE PROFILE   generated {p['generated']}")
    add(f"input: {p['input']}")
    add("=" * 78)

    st = p["stream"]
    add("\n[STREAM]")
    kv("datagrams / valid", f"{st['datagrams']} / {st['valid']}")
    kv("sources", ", ".join(f"{k} ({v})" for k, v in st["sources"].items()) or "-")
    if st["rejected"]:
        kv("rejected", ", ".join(f"{k}: {v}" for k, v in st["rejected"].items()))
    if st["valid"]:
        sq = st["sequence"]
        kv("sequence first..last", f"{sq['first']}..{sq['last']}")
        kv("lost / dup / reordered", f"{sq['lost']} / {sq['duplicates']} / {sq['reordered']} "
                                    f"({sq['loss_pct']}% loss)")
        kv("boot sessions", st["boot_sessions"])
        kv("uptime first..last", f"{st['uptime_ms']['first'] / 1000:.3f}s .. "
                                 f"{st['uptime_ms']['last'] / 1000:.3f}s")
        if st["interval_ms"]:
            i = st["interval_ms"]
            kv("sample interval ms", f"min {i['min']} / mean {i['mean']} / max {i['max']}"
                                     + (f" / stdev {i['stdev']}" if "stdev" in i else ""))

    n = p["network"]
    add("\n[1. NETWORK STACK & CONTROLLER]")
    if n["pci"]:
        kv("PCI location", n["pci"]["bdf"])
        kv("vendor:device", f"{n['pci']['vendor']}:{n['pci']['device']} (Realtek RTL8168/8111)")
    else:
        kv("PCI identification", "no serial log line (pass --serial-log)")
    for name, b in n["bars"].items():
        kv(name, f"{b['type']:<4} {b['base']}-{b['end']} (size {b['size']})")
    kv("MAC address", n["mac"] or "no serial log line (pass --serial-log)")
    for s in n["driver_status"]:
        kv("driver", s)
    if n["ip_config"]:
        kv("IPv4", f"{n['ip_config']['ip']} mask {n['ip_config']['mask']} gw {n['ip_config']['gateway']}")
    kv("link (PHY)", n["link_state"]["phy_register"])
    kv("link (inferred)", n["link_state"]["inferred"] or "no valid samples")
    kv("NIC running flag", f"{n['link_state']['nic_running_flag_pct']}% of samples")
    tc = n["telemetry_counters"]
    if tc:
        add("  counters            absolute     delta      rate/s")
        for c in COUNTERS:
            add(f"    {c:<16} {tc['last_absolute'][c]:>10} {tc['delta_over_capture'][c]:>9} "
                f"{tc['rate_per_s'][c] if tc['rate_per_s'][c] is not None else '-':>11}")
        kv("TX drop / timeout rate", f"{tc['tx_drop_pct']}% / {tc['tx_timeout_pct']}%")
        kv("RX drop rate", f"{tc['rx_drop_pct']}%")
        kv("note", tc["note"])
    if n["netstat_snapshot"]:
        kv("NETSTAT (serial)", ", ".join(f"{k}={v}" for k, v in n["netstat_snapshot"].items()))
    for e in n["net_events"][-10:]:
        kv("net event", e)

    a = p["audio"]
    add("\n[2. SOUND / AUDIO SUBSYSTEM]")
    for k in ("driver", "hardware_identification", "interrupt_configuration", "buffer_allocation"):
        kv(k, a[k])
    kv("audio telemetry log entries", ", ".join(a["telemetry_log_entries"]) or "none")
    kv("audio serial log entries", len(a["serial_log_entries"]))
    for line in a["serial_log_entries"][:10]:
        add(f"      {line}")

    c = p["cpu_chipset"]
    add("\n[3. CPU, CHIPSET & MEMORY]")
    if c["cpu_mhz"]:
        kv("CPU MHz", f"{c['cpu_mhz']['last']} (min {c['cpu_mhz']['min']} / max {c['cpu_mhz']['max']})")
        kv("", c["cpu_mhz_note"])
    if c["flags_last"]:
        f = c["flags_last"]
        kv("CPU flags (last)", f"{f['raw']} desktop={f['desktop_active']} "
                               f"irq={f['interrupts_enabled']} nic={f['nic_running']}")
        pct = c["flags_pct_of_samples"]
        kv("flags % of samples", f"desktop {pct['desktop_active']}% / irq {pct['interrupts_enabled']}% "
                                 f"/ nic {pct['nic_running']}%")
    if c["unknown_flag_bits_seen"]:
        kv("unknown flag bits", ", ".join(c["unknown_flag_bits_seen"]))
    kv("core states", c["core_states"])
    kv("power mgmt registers", c["power_management_registers"])
    for line in c["acpi_from_serial"]:
        kv("ACPI (serial)", line)
    if c["framebuffer_from_serial"]:
        kv("framebuffer (serial)", c["framebuffer_from_serial"])
    m = p["memory"]
    if m:
        kv("physical memory", f"{m['total_bytes']} B ({m['total_mib']} MiB)")
        kv("kernel reserved", f"{m['kernel_reserved_bytes']} B ({m['kernel_reserved_mib']} MiB)")
        kv("outside kernel", f"{m['available_outside_kernel_bytes']} B")
        h = m["heap_used_bytes"]
        kv("heap used", f"{h['last']} / {m['heap_total_bytes']} B ({m['heap_used_pct_last']}%), "
                        f"min {h['min']} max {h['max']}")
        kv("heap growth (this boot)", "-" if m["heap_growth_bytes_per_s_current_boot"] is None
           else f"{m['heap_growth_bytes_per_s_current_boot']} B/s")
        if m["values_changed_during_capture"]:
            kv("WARNING changed fields", ", ".join(m["values_changed_during_capture"]))
        kv("note", m["note"])

    d = p.get("diagnostics_v2")
    add("\n[4. V2 DIAGNOSTICS (build, health, rings, driver loop)]")
    if not d:
        add("  no v2 packets (kernel predates telemetry v2)")
    else:
        kv("v2 samples", d["samples"])
        kv("build / relaunch", f"{d['build_id']} / #{d['relaunch_count']}")
        if len(d["build_history"]) > 1:
            kv("build changes", " -> ".join(f"{b['build_id']}#{b['relaunch_count']}"
                                            for b in d["build_history"]))
        kv("health (last)", d["health_flags_last"])
        kv("PHYstatus values", ", ".join(d["phy_status_values"]))
        kv("link-down samples", d["link_down_samples"])
        for key in ("tx_ring_pending", "rx_ring_ready", "poll_iterations_per_sample",
                    "poll_avg_us", "poll_max_us"):
            s = d[key]
            kv(key, f"last {s['last']} (min {s['min']} / mean {s['mean']} / max {s['max']})")
        kv("stack counters", ", ".join(f"{k}={v}" for k, v in d["stack_counters_last"].items()))
        kv("telemetry send failures", d["telemetry_failures_last"])
        kv("push accepted / rejected", f"{d['push_accepted_last']} / {d['push_rejected_last']}")

    add("\n[LOG BUFFER MESSAGES]")
    for msg, cnt in sorted(p["log_messages"].items(), key=lambda x: -x[1]):
        kv(repr(msg), cnt)
    if not p["log_messages"]:
        add("  none")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Build a BOOK8 hardware profile from telemetry")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--live", type=float, metavar="SECONDS",
                     help="capture UDP telemetry for SECONDS (stop telemetry_listener.py first)")
    src.add_argument("--raw", metavar="FILE", help="JSON-lines capture from telemetry_listener.py --raw-out")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=TELEMETRY_PORT)
    ap.add_argument("--source", default=None, help="only accept this sender IP")
    ap.add_argument("--serial-log", action="append", default=[], metavar="FILE",
                    help="BOOK8 serial/console capture (repeatable; later files override "
                         "earlier single-value fields such as MAC and NETSTAT)")
    ap.add_argument("--report", default=None, help="write the text report to FILE")
    ap.add_argument("--json", default=None, help="write the structured profile to FILE")
    args = ap.parse_args()

    if args.live is not None:
        print(f"[*] capturing UDP {args.bind}:{args.port} for {args.live:g}s "
              f"(expect {PACKET_SIZE}-byte v1 or {PACKET_SIZE_V2}-byte v2 packets)...")
        records = capture_live(args.bind, args.port, args.live, args.source)
        desc = f"live {args.bind}:{args.port} for {args.live:g}s"
    else:
        records = load_raw(args.raw, args.source)
        desc = f"raw capture {args.raw}"
    if args.source:
        desc += f", source {args.source}"
    serial = parse_serial(args.serial_log) if args.serial_log else None
    if serial:
        desc += f", serial logs: {', '.join(args.serial_log)}"

    stream, samples = analyze_stream(records)
    profile = build_profile(stream, samples, serial, desc)
    text = render(profile)
    print(text, end="")
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(text)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(profile, f, indent=2)
    if not samples:
        print("[!] No valid BOOK8 telemetry received. On BOOK8 run NETCONFIG and "
              "TELEMETRY <this-host-ip>; allow inbound UDP 8080 in Windows Firewall.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
