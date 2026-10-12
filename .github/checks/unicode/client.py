#!/usr/bin/env python3
"""Run the independent libnfs client against real SdFat image-backed exports."""
import socket
import subprocess
import sys

for filesystem in ("fat", "exfat"):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen([sys.argv[1], filesystem, str(port)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        ready = server.stdout.readline().strip().split()
        assert ready[0] == "READY"
        subprocess.run([sys.argv[2], str(port), ready[2]], check=True, timeout=60)
    finally:
        server.terminate()
        _, errors = server.communicate(timeout=15)
        assert server.returncode == 0 and not errors, errors
