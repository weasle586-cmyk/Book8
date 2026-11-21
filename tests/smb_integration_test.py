#!/usr/bin/env python3
"""Validate BOOK8 TCP/SMB against disposable loopback-only SMB2 servers.

Requires impacket in the invoking interpreter. No physical/network shares used.
usage: smb_integration_test.py [host-test-binary] [log-directory]
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def server(port, directory, authenticated):
    from impacket.smbserver import SimpleSMBServer
    instance = SimpleSMBServer(listenAddress="127.0.0.1", listenPort=int(port))
    instance.setSMB2Support(True)
    if authenticated:
        from impacket.ntlm import compute_lmhash, compute_nthash
        instance.addCredential("tester", 1, compute_lmhash("synthetic-pass").hex(),
                               compute_nthash("synthetic-pass").hex())
    instance.addShare("public", directory, readOnly="no" if authenticated else "yes")
    instance.start()


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--server":
        server(sys.argv[2], sys.argv[3], sys.argv[4] == "auth")
        return
    from impacket.smbserver import SimpleSMBServer
    assert callable(SimpleSMBServer)
    root = Path(__file__).resolve().parent.parent
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build" / "smb_integration_host"
    logs = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root / "build" / "netshare-validation"
    if not binary.is_file():
        raise SystemExit("Compile tests/smb_integration_host.c first")
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-smb-fixture-") as temp:
        directory = Path(temp)
        (directory / "docs").mkdir()
        source = directory / "docs" / "test.c"
        source.write_bytes(b"int main(void) { return 0; }\n" * 2048)
        with (directory / "large.bin").open("wb") as large:
            large.truncate(1024 * 1024 + 1)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        for mode in ("guest", "auth"):
            with (logs / f"{mode}-server.log").open("wb") as log:
                process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                            "--server", str(port), str(directory), mode],
                                           stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 20
                    while True:
                        if process.poll() is not None:
                            raise AssertionError(f"Local SMB test server exited; see {mode}-server.log")
                        try:
                            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                                break
                        except (ConnectionRefusedError, TimeoutError):
                            if time.monotonic() >= deadline:
                                raise AssertionError("Local SMB server did not become ready")
                            time.sleep(0.1)
                    args = [str(binary), str(port), str(source)]
                    if mode == "auth":
                        args.append("auth")
                    result = subprocess.run(args, capture_output=True, timeout=120)
                    (logs / f"{mode}-client.log").write_bytes(result.stdout + result.stderr)
                    print(result.stdout.decode(errors="replace"), end="")
                    if result.returncode:
                        raise AssertionError(result.stderr.decode(errors="replace"))
                    if source.read_bytes() != b"int main(void) { return 0; }\n" * 2048:
                        raise AssertionError("Source fixture was modified")
                    if mode == "auth":
                        if (directory / "docs" / "upload.c").read_bytes() != source.read_bytes():
                            raise AssertionError("Uploaded bytes differ from the local fixture")
                        if (directory / "empty.bin").read_bytes() != b"":
                            raise AssertionError("Empty upload has unexpected data")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f"PASS: isolated SMB integration; logs: {logs}")


if __name__ == "__main__":
    main()
