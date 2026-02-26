#!/usr/bin/env python3
"""
exo_nettest.py — Host-side UDP + TCP test server for EXO_OS network testing.

Run this on the host before using udptest / tcpconn in the EXO_OS shell.

  python3 tools/exo_nettest.py

Then in the EXO_OS shell:
  udptest 10.0.2.2 5555 hello world
  tcpconn 10.0.2.2 5556
"""

import socket
import threading
import time
import sys
import argparse

UDP_PORT = 5555
TCP_PORT = 5556
TCP_RESPONSE = b"Hello from host!\r\n"


# ── ANSI colours ──────────────────────────────────────────────────────────────
R  = "\033[0m"
GR = "\033[1;32m"
CY = "\033[1;36m"
YL = "\033[1;33m"
RD = "\033[1;31m"


def ts() -> str:
    return time.strftime("%H:%M:%S")


# ── UDP server ────────────────────────────────────────────────────────────────
def udp_server(host: str, port: int, stop: threading.Event) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1.0)
    sock.bind((host, port))
    print(f"{GR}[{ts()}] UDP  listening on {host}:{port}{R}")

    while not stop.is_set():
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break

        msg = data.decode("utf-8", errors="replace").rstrip("\r\n")
        print(f"{CY}[{ts()}] UDP  rx  {addr[0]}:{addr[1]}  →  \"{msg}\"{R}")

        # echo back with a prefix so the guest can see the reply
        reply = f"ECHO:{msg}".encode()
        sock.sendto(reply, addr)
        print(f"{YL}[{ts()}] UDP  tx  {addr[0]}:{addr[1]}  ←  \"{reply.decode()}\"{R}")

    sock.close()
    print(f"[{ts()}] UDP  server stopped.")


# ── TCP server ────────────────────────────────────────────────────────────────
def tcp_handle_client(conn: socket.socket, addr: tuple) -> None:
    print(f"{GR}[{ts()}] TCP  connection from {addr[0]}:{addr[1]}{R}")
    conn.settimeout(5.0)
    try:
        data = b""
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            data += chunk
            # stop reading once we have a complete line
            if b"\n" in data:
                break

        msg = data.decode("utf-8", errors="replace").rstrip("\r\n")
        print(f"{CY}[{ts()}] TCP  rx  {addr[0]}:{addr[1]}  →  \"{msg}\"{R}")

        conn.sendall(TCP_RESPONSE)
        print(f"{YL}[{ts()}] TCP  tx  {addr[0]}:{addr[1]}  ←  \"{TCP_RESPONSE.decode().rstrip()}\"{R}")
    except socket.timeout:
        print(f"{RD}[{ts()}] TCP  timeout reading from {addr[0]}:{addr[1]}{R}")
    except Exception as e:
        print(f"{RD}[{ts()}] TCP  error: {e}{R}")
    finally:
        conn.close()
        print(f"[{ts()}] TCP  connection closed {addr[0]}:{addr[1]}")


def tcp_server(host: str, port: int, stop: threading.Event) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1.0)
    sock.bind((host, port))
    sock.listen(8)
    print(f"{GR}[{ts()}] TCP  listening on {host}:{port}{R}")

    while not stop.is_set():
        try:
            conn, addr = sock.accept()
        except socket.timeout:
            continue
        except OSError:
            break

        t = threading.Thread(target=tcp_handle_client, args=(conn, addr), daemon=True)
        t.start()

    sock.close()
    print(f"[{ts()}] TCP  server stopped.")


# ── main ──────────────────────────────────────────────────────────────────────
def main() -> None:
    parser = argparse.ArgumentParser(description="EXO_OS network test server")
    parser.add_argument("--host",     default="0.0.0.0", help="bind address (default: 0.0.0.0)")
    parser.add_argument("--udp-port", type=int, default=UDP_PORT, help=f"UDP port (default: {UDP_PORT})")
    parser.add_argument("--tcp-port", type=int, default=TCP_PORT, help=f"TCP port (default: {TCP_PORT})")
    args = parser.parse_args()

    print(f"\n{GR}EXO_OS network test server{R}")
    print(f"  UDP echo  →  port {args.udp_port}")
    print(f"  TCP echo  →  port {args.tcp_port}")
    print(f"  Press Ctrl-C to stop.\n")

    stop = threading.Event()

    udp_t = threading.Thread(target=udp_server,
                              args=(args.host, args.udp_port, stop), daemon=True)
    tcp_t = threading.Thread(target=tcp_server,
                              args=(args.host, args.tcp_port, stop), daemon=True)
    udp_t.start()
    tcp_t.start()

    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        print(f"\n[{ts()}] Shutting down...")
        stop.set()

    udp_t.join(timeout=3)
    tcp_t.join(timeout=3)
    print("Done.")


if __name__ == "__main__":
    main()
