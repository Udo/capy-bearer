#!/usr/bin/env python3
"""Exercise real nginx/FastCGI text, binary, and close paths for a Capy WS handler."""

import base64
import os
import socket
import struct

HOST = "bearer.openfu.com"
PATH = "/tests/capy-websocket.capy"


def receive_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("WebSocket closed before the expected frame")
        data.extend(chunk)
    return bytes(data)


def receive_frame(sock: socket.socket) -> tuple[int, bytes]:
    first, second = receive_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", receive_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", receive_exact(sock, 8))[0]
    return first & 0x0F, receive_exact(sock, length)


def exchange(opcode: int, payload: bytes, expected: list[tuple[int, bytes]]) -> None:
    with socket.create_connection(("127.0.0.1", 80), timeout=10) as connection:
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {PATH} HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        connection.sendall(request.encode())
        response = bytearray()
        while b"\r\n\r\n" not in response:
            response.extend(connection.recv(4096))
        if not response.startswith(b"HTTP/1.1 101 "):
            raise SystemExit(f"Capy WebSocket upgrade failed: {response[:200]!r}")

        mask = b"\x12\x34\x56\x78"
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        connection.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)
        frames = [receive_frame(connection) for _ in expected]
        if frames != expected:
            raise SystemExit(f"Capy WebSocket frame mismatch: {frames!r}, expected {expected!r}")


exchange(1, b"hello", [(1, b"echo:hello"), (1, b"direct:hello"), (8, b"\x03\xe8")])
exchange(2, b"\x00\xffcapy", [(2, b"\x00\xffcapy"), (8, b"\x03\xe8")])
print("Capy WebSocket text/binary inspect/send/close path passed")
