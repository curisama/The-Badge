"""A very small WebSocket implementation (RFC6455), written out here so that no
outside package is needed.

Only what is needed: the handshake, reading and writing text and binary frames,
unmasking. Fragmented frames (continuation) and extensions are not used — we
make everything we send."""
import base64, hashlib, os, select, struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def accept_key(client_key):
    return base64.b64encode(hashlib.sha1((client_key + GUID).encode()).digest()).decode()


def handshake(handler):
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key:
        return False
    handler.send_response(101)
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept_key(key))
    handler.end_headers()
    return True


def send(sock, payload, opcode=0x2):
    """opcode 0x1=text 0x2=binary 0x8=close"""
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n < (1 << 16):
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    sock.sendall(head + payload)


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def recv(sock):
    """One frame. None on close."""
    head = _recv_exact(sock, 2)
    if not head:
        return None
    opcode = head[0] & 0x0F
    masked = head[1] & 0x80
    n = head[1] & 0x7F
    if n == 126:
        n = struct.unpack("!H", _recv_exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", _recv_exact(sock, 8))[0]
    mask = _recv_exact(sock, 4) if masked else None
    data = _recv_exact(sock, n) if n else b""
    if data is None:
        return None
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    if opcode == 0x8:
        return None
    return data


def pending(sock, timeout=0):
    return bool(select.select([sock], [], [], timeout)[0])
