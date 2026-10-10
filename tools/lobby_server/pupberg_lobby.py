#!/usr/bin/env python3
"""PupBerg lobby server

Lets PupBerg players find each other over the internet with a room code, no VPN needed.
Every client keeps one TCP connection to this server; the server tells room members
about each other and relays their emu messages, so it works behind any NAT.

Frame format (both directions): u32 length (little endian, covers type + payload), u8 type, payload
Strings are u8 length + utf-8 bytes.

client -> server
  HELLO  0x01  u16 version, u64 steamid, u32 appid, str room, str name, u8 flags (bit 0 = public room),
               u8 count + u64 extra ids (ex: the player's game server id), optional
  DATA   0x02  u64 dest id (any id of a member, 0 = everyone in the room), bytes payload
  PING   0x03
  LIST   0x04  u32 appid (0 = any game), may be sent before HELLO
server -> client
  WELCOME 0x81 u16 version, u32 observed public ipv4 (network order)
  JOIN    0x82 u64 steamid, u32 appid, str name, u8 count + u64 extra ids
  LEAVE   0x83 u64 steamid
  DATA    0x84 u64 source steamid, bytes payload
  PONG    0x85
  ERROR   0x86 str message (the server closes the connection right after)
  ROOMS   0x87 u16 count, then per room: str code, str host name, u8 members, u32 appid

The first member decides if a room is public; public rooms show up in LIST, private
ones can only be joined with their code.
"""

import argparse
import asyncio
import ipaddress
import logging
import re
import socket
import struct
import time

PROTOCOL_VERSION = 1

HELLO, DATA, PING, LIST = 0x01, 0x02, 0x03, 0x04
WELCOME, JOIN, LEAVE, RDATA, PONG, ERROR, ROOMS = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
FLAG_PUBLIC = 0x01
MAX_LISTED_ROOMS = 200

MAX_FRAME = 1024 * 1024
HELLO_TIMEOUT = 10.0
IDLE_TIMEOUT = 45.0
ROOM_RE = re.compile(r"^[A-Za-z0-9_-]{4,32}$")

log = logging.getLogger("pupberg-lobby")


def pack_str(s: str) -> bytes:
    b = s.encode("utf-8")[:255]
    return bytes([len(b)]) + b


def read_str(buf: bytes, off: int):
    n = buf[off]
    off += 1
    if off + n > len(buf):
        raise ValueError("string out of bounds")
    return buf[off:off + n].decode("utf-8", "replace"), off + n


def frame(ftype: int, payload: bytes = b"") -> bytes:
    return struct.pack("<IB", len(payload) + 1, ftype) + payload


class Client:
    def __init__(self, server, reader, writer):
        self.server = server
        self.reader = reader
        self.writer = writer
        self.peer_ip = writer.get_extra_info("peername")[0]
        self.steamid = 0
        self.appid = 0
        self.name = ""
        self.extra_ids = []
        self.room = None

    def join_frame(self):
        extra = struct.pack("<B", len(self.extra_ids)) + b"".join(struct.pack("<Q", i) for i in self.extra_ids)
        return frame(JOIN, struct.pack("<QI", self.steamid, self.appid) + pack_str(self.name) + extra)

    def owns(self, steamid):
        return steamid == self.steamid or steamid in self.extra_ids

    def send(self, data: bytes):
        if self.writer.is_closing():
            return
        # a client that stops reading would make us buffer forever
        if self.writer.transport.get_write_buffer_size() > self.server.max_buffer:
            log.info("dropping slow client %s in room %s", self.steamid, self.room)
            self.writer.close()
            return
        self.writer.write(data)

    async def read_frame(self, timeout):
        head = await asyncio.wait_for(self.reader.readexactly(5), timeout)
        length, ftype = struct.unpack("<IB", head)
        if length < 1 or length > MAX_FRAME:
            raise ValueError(f"bad frame length {length}")
        payload = await asyncio.wait_for(self.reader.readexactly(length - 1), timeout)
        return ftype, payload

    async def error(self, message):
        self.send(frame(ERROR, pack_str(message)))
        try:
            await self.writer.drain()
        except ConnectionError:
            pass


class Room:
    def __init__(self, code, public, host_name):
        self.code = code
        self.public = public
        self.host_name = host_name
        self.members = {}  # steamid -> Client


class LobbyServer:
    def __init__(self, max_clients, max_per_ip, max_room, max_buffer):
        self.rooms = {}  # code -> Room
        self.clients = set()
        self.per_ip = {}
        self.max_clients = max_clients
        self.max_per_ip = max_per_ip
        self.max_room = max_room
        self.max_buffer = max_buffer
        self.started = time.time()
        self.relayed_bytes = 0

    async def handle(self, reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(self, reader, writer)

        if len(self.clients) >= self.max_clients or self.per_ip.get(c.peer_ip, 0) >= self.max_per_ip:
            await c.error("server is full")
            writer.close()
            return

        self.clients.add(c)
        self.per_ip[c.peer_ip] = self.per_ip.get(c.peer_ip, 0) + 1
        try:
            await self.session(c)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError):
            pass
        except ValueError as e:
            log.info("protocol error from %s: %s", c.peer_ip, e)
        finally:
            self.leave(c)
            self.clients.discard(c)
            self.per_ip[c.peer_ip] -= 1
            if not self.per_ip[c.peer_ip]:
                del self.per_ip[c.peer_ip]
            writer.close()

    def room_list(self, appid):
        out = []
        for r in self.rooms.values():
            if not r.public or not r.members:
                continue
            host = next(iter(r.members.values()))
            if appid and host.appid != appid:
                continue
            out.append(pack_str(r.code) + pack_str(r.host_name) + struct.pack("<BI", min(len(r.members), 255), host.appid))
            if len(out) >= MAX_LISTED_ROOMS:
                break
        return frame(ROOMS, struct.pack("<H", len(out)) + b"".join(out))

    async def session(self, c: Client):
        while True:
            ftype, p = await c.read_frame(HELLO_TIMEOUT)
            if ftype == LIST and len(p) >= 4:
                c.send(self.room_list(struct.unpack_from("<I", p, 0)[0]))
                continue
            break
        if ftype != HELLO or len(p) < 14:
            raise ValueError("expected HELLO")
        version, c.steamid, c.appid = struct.unpack_from("<HQI", p, 0)
        room, off = read_str(p, 14)
        c.name, off = read_str(p, off)
        flags = p[off] if off < len(p) else 0
        off += 1
        if off < len(p):
            n = min(p[off], 8)
            off += 1
            for _ in range(n):
                if off + 8 > len(p):
                    break
                extra = struct.unpack_from("<Q", p, off)[0]
                off += 8
                if extra and extra != c.steamid:
                    c.extra_ids.append(extra)
        if version != PROTOCOL_VERSION:
            await c.error(f"unsupported protocol version {version}, update PupBerg")
            return
        if not ROOM_RE.match(room):
            await c.error("invalid room code")
            return
        if not c.steamid:
            await c.error("invalid steam id")
            return
        room = room.upper()

        r = self.rooms.get(room)
        if r is None:
            r = self.rooms[room] = Room(room, bool(flags & FLAG_PUBLIC), c.name)
        members = r.members
        old = members.get(c.steamid)
        if old is None and len(members) >= self.max_room:
            await c.error("room is full")
            return
        if old is not None:
            # same account reconnecting (or a duplicate id), the newest connection wins
            old.room = None
            del members[c.steamid]
            for m in members.values():
                m.send(frame(LEAVE, struct.pack("<Q", c.steamid)))
            old.writer.close()

        ip = 0
        try:
            addr = ipaddress.ip_address(c.peer_ip)
            if addr.version == 6 and addr.ipv4_mapped:
                addr = addr.ipv4_mapped
            if addr.version == 4:
                ip = int(addr)
        except ValueError:
            pass
        c.send(frame(WELCOME, struct.pack("<HI", PROTOCOL_VERSION, ip)))

        join = c.join_frame()
        for m in members.values():
            m.send(join)
            c.send(m.join_frame())
        members[c.steamid] = c
        c.room = room
        log.info("%s (%s) joined %s room %s [%d members]", c.name, c.steamid, "public" if r.public else "private", room, len(members))

        while True:
            ftype, p = await c.read_frame(IDLE_TIMEOUT)
            if c.room is None:
                return  # replaced by a newer connection
            if ftype == PING:
                c.send(frame(PONG))
            elif ftype == LIST and len(p) >= 4:
                c.send(self.room_list(struct.unpack_from("<I", p, 0)[0]))
            elif ftype == DATA:
                if len(p) < 8:
                    raise ValueError("short DATA")
                dest, = struct.unpack_from("<Q", p, 0)
                out = frame(RDATA, struct.pack("<Q", c.steamid) + p[8:])
                members = r.members
                if dest:
                    target = members.get(dest)
                    if target is None:
                        target = next((m for m in members.values() if m.owns(dest)), None)
                    if target is not None and target is not c:
                        target.send(out)
                        self.relayed_bytes += len(out)
                else:
                    for m in members.values():
                        if m is not c:
                            m.send(out)
                            self.relayed_bytes += len(out)
            else:
                raise ValueError(f"unknown frame type {ftype}")

    def leave(self, c: Client):
        if c.room is None:
            return
        r = self.rooms.get(c.room)
        members = r.members if r else {}
        if members.get(c.steamid) is c:
            del members[c.steamid]
            msg = frame(LEAVE, struct.pack("<Q", c.steamid))
            for m in members.values():
                m.send(msg)
            log.info("%s (%s) left %s [%d members]", c.name, c.steamid, c.room, len(members))
        if not members:
            self.rooms.pop(c.room, None)
        c.room = None

    async def report(self, interval):
        while True:
            await asyncio.sleep(interval)
            log.info("stats: %d clients, %d rooms, %.1f MiB relayed",
                     len(self.clients), len(self.rooms), self.relayed_bytes / 1048576)


async def main():
    ap = argparse.ArgumentParser(description="PupBerg lobby server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=47620)
    ap.add_argument("--max-clients", type=int, default=512)
    ap.add_argument("--max-per-ip", type=int, default=16)
    ap.add_argument("--max-room", type=int, default=64)
    ap.add_argument("--max-buffer", type=int, default=8 * 1024 * 1024, help="max queued bytes per client")
    ap.add_argument("--stats-interval", type=float, default=3600.0)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    server = LobbyServer(args.max_clients, args.max_per_ip, args.max_room, args.max_buffer)
    srv = await asyncio.start_server(server.handle, args.host, args.port)
    log.info("listening on %s:%d", args.host, args.port)
    asyncio.create_task(server.report(args.stats_interval))
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
