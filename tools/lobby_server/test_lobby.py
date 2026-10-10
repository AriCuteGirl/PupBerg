#!/usr/bin/env python3
"""Protocol test for pupberg_lobby.py: starts a server on a free port and plays a few clients against it."""

import asyncio
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pupberg_lobby as L  # noqa: E402


class TestClient:
    def __init__(self, port):
        self.port = port

    async def connect(self):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", self.port)

    def send(self, ftype, payload=b""):
        self.w.write(L.frame(ftype, payload))

    def hello(self, steamid, appid, room, name, public=False, extra=()):
        self.send(L.HELLO, struct.pack("<HQI", L.PROTOCOL_VERSION, steamid, appid)
                  + L.pack_str(room) + L.pack_str(name) + bytes([L.FLAG_PUBLIC if public else 0])
                  + bytes([len(extra)]) + b"".join(struct.pack("<Q", i) for i in extra))

    async def recv(self, timeout=2.0):
        head = await asyncio.wait_for(self.r.readexactly(5), timeout)
        length, ftype = struct.unpack("<IB", head)
        return ftype, await asyncio.wait_for(self.r.readexactly(length - 1), timeout)

    async def expect(self, ftype):
        t, p = await self.recv()
        assert t == ftype, f"expected {ftype:#x}, got {t:#x}"
        return p


def parse_rooms(p):
    n, = struct.unpack_from("<H", p, 0)
    off, rooms = 2, []
    for _ in range(n):
        code, off = L.read_str(p, off)
        host, off = L.read_str(p, off)
        members, appid = struct.unpack_from("<BI", p, off)
        off += 5
        rooms.append((code, host, members, appid))
    return rooms


async def run():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = L.LobbyServer(64, 16, 8, 1 << 20)
    srv = await asyncio.start_server(server.handle, "127.0.0.1", port)

    a, b, c, d = (TestClient(port) for _ in range(4))
    for x in (a, b, c, d):
        await x.connect()

    # a creates a public room, b joins it
    a.hello(1001, 480, "pupparty", "Alice", public=True)
    await a.expect(L.WELCOME)
    b.hello(1002, 480, "PUPPARTY", "Bob", extra=(85568392920039424,))
    await b.expect(L.WELCOME)
    p = await b.expect(L.JOIN)
    assert struct.unpack_from("<QI", p)[0] == 1001
    p = await a.expect(L.JOIN)
    assert struct.unpack_from("<QI", p)[0] == 1002
    name, off = L.read_str(p, 12)
    assert name == "Bob" and p[off] == 1 and struct.unpack_from("<Q", p, off + 1)[0] == 85568392920039424
    print("ok: join + member lists + extra ids")

    # c makes a private room for another game
    c.hello(2001, 730, "secret1", "Carol")
    await c.expect(L.WELCOME)

    # d browses before joining anything
    d.send(L.LIST, struct.pack("<I", 0))
    rooms = parse_rooms(await d.expect(L.ROOMS))
    assert rooms == [("PUPPARTY", "Alice", 2, 480)], rooms
    d.send(L.LIST, struct.pack("<I", 730))
    assert parse_rooms(await d.expect(L.ROOMS)) == []
    print("ok: only public rooms are listed, appid filter works")

    # direct and broadcast relay
    a.send(L.DATA, struct.pack("<Q", 1002) + b"hello bob")
    p = await b.expect(L.RDATA)
    assert struct.unpack_from("<Q", p)[0] == 1001 and p[8:] == b"hello bob"
    a.send(L.DATA, struct.pack("<Q", 85568392920039424) + b"to bob's server")
    p = await b.expect(L.RDATA)
    assert p[8:] == b"to bob's server"
    b.send(L.DATA, struct.pack("<Q", 0) + b"hi all")
    p = await a.expect(L.RDATA)
    assert p[8:] == b"hi all"
    # messages never cross rooms
    a.send(L.DATA, struct.pack("<Q", 2001) + b"leak?")
    try:
        await c.recv(0.5)
        raise AssertionError("message crossed rooms")
    except asyncio.TimeoutError:
        pass
    print("ok: relay direct + broadcast, no cross-room delivery")

    a.send(L.PING)
    await a.expect(L.PONG)

    # same steamid reconnecting replaces the old connection
    b2 = TestClient(port)
    await b2.connect()
    b2.hello(1002, 480, "pupparty", "Bob")
    await b2.expect(L.WELCOME)
    assert struct.unpack_from("<Q", await a.expect(L.LEAVE))[0] == 1002
    assert struct.unpack_from("<Q", await a.expect(L.JOIN))[0] == 1002
    print("ok: reconnect replaces the old session")

    # leaving
    b2.w.close()
    assert struct.unpack_from("<Q", await a.expect(L.LEAVE))[0] == 1002
    print("ok: leave notifications")

    # bad input
    e = TestClient(port)
    await e.connect()
    e.hello(3001, 480, "no", "Eve")
    p = await e.expect(L.ERROR)
    assert "room" in L.read_str(p, 0)[0]
    print("ok: invalid room code rejected")

    for x in (a, c, d):
        x.w.close()
    srv.close()
    await srv.wait_closed()
    print("ALL OK")


asyncio.run(run())
