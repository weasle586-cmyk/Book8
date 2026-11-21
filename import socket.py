import socket
import struct
import datetime

# Must match book8 kernel definitions
TELEMETRY_PORT = 8080
MAGIC_EXPECTED = 0x424B3801  # 'BK81'
TYPE_METRICS = 0x01
TYPE_LOG = 0x02

# Header struct layout: magic (4B), seq (4B), timestamp (8B), type (1B), flags (1B), length (2B)
HEADER_FORMAT = "!IIQBBH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

# Metrics struct layout: cpu_active (4B), cpu_idle (4B), heap_free (4B), active_sockets (4B), rx_dropped (4B), tx_errors (4B)
METRICS_FORMAT = "!IIIIII"
METRICS_SIZE = struct.calcsize(METRICS_FORMAT)

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", TELEMETRY_PORT))
    print(f"[*] book8 Telemetry Listener active on UDP port {TELEMETRY_PORT}...")

    log_file = open("book8_telemetry.log", "a")

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            if len(data) < HEADER_SIZE:
                continue

            # Unpack packet header
            magic, seq, _timestamp, ptype, _flags, length = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])

            if magic != MAGIC_EXPECTED:
                print(f"[!] Warning: Invalid magic 0x{magic:08X} from {addr[0]}")
                continue

            now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            payload = data[HEADER_SIZE:HEADER_SIZE + length]

            if ptype == TYPE_METRICS and len(payload) >= METRICS_SIZE:
                active, idle, heap, sockets, rx_drop, tx_err = struct.unpack(METRICS_FORMAT, payload[:METRICS_SIZE])
                total_ticks = active + idle if (active + idle) > 0 else 1
                cpu_load = (active / total_ticks) * 100

                log_entry = (f"[{now}] [METRICS] Seq: {seq} | CPU Load: {cpu_load:.1f}% | "
                             f"Heap Free: {heap / 1024:.1f} KB | Sockets: {sockets} | "
                             f"RX Drop: {rx_drop} | TX Err: {tx_err}")
                print(log_entry)
                log_file.write(log_entry + "\n")
                log_file.flush()

            elif ptype == TYPE_LOG:
                try:
                    log_msg = payload.decode('utf-8').rstrip('\x00')
                except UnicodeDecodeError:
                    log_msg = str(payload)

                log_entry = f"[{now}] [KERNEL LOG] Seq: {seq} | {log_msg}"
                print(log_entry)
                log_file.write(log_entry + "\n")
                log_file.flush()

    except KeyboardInterrupt:
        print("\n[*] Telemetry listener shut down.")
    finally:
        log_file.close()
        sock.close()

if __name__ == "__main__":
    main()