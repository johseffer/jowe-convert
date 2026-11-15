#!/usr/bin/env python3
"""Self-test for the jvconvert MCP server.

Starts `jvconvert mcp --allow <temp dir>` over stdio, sends initialize, tools/list
and tools/call requests, and checks the files it writes. The inputs (a PNG, a WAV
and an OBJ cube) are generated here. Standard library only; no network access.

Usage: python3 mcp_selftest.py /path/to/jvconvert [--keep]
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave
import zlib


def make_png(path, w=320, h=200):
    rows = b""
    for y in range(h):
        rows += b"\x00" + bytes(
            v for x in range(w) for v in (x * 255 // w, y * 255 // h, 128, 255)
        )

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make_wav(path, rate=44100, seconds=1.0):
    n = int(rate * seconds)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            s = int(12000 * math.sin(2 * math.pi * 440 * i / rate))
            frames += struct.pack("<hh", s, s)
        w.writeframes(bytes(frames))


def make_obj(path):
    v = [(x, y, z) for x in (0, 1) for y in (0, 1) for z in (0, 1)]
    faces = [(1, 3, 4, 2), (5, 6, 8, 7), (1, 2, 6, 5), (3, 7, 8, 4), (1, 5, 7, 3), (2, 4, 8, 6)]
    mtl = os.path.splitext(path)[0] + ".mtl"
    with open(mtl, "w") as f:
        f.write("newmtl red\nKd 0.8 0.1 0.1\n")
    with open(path, "w") as f:
        f.write("mtllib %s\n" % os.path.basename(mtl))
        for p in v:
            f.write("v %g %g %g\n" % p)
        f.write("usemtl red\n")
        for face in faces:
            f.write("f %d %d %d %d\n" % face)


MAGIC = {
    ".webp": lambda b: b[:4] == b"RIFF" and b[8:12] == b"WEBP",
    ".jpg": lambda b: b[:3] == b"\xff\xd8\xff",
    ".flac": lambda b: b[:4] == b"fLaC",
    ".wav": lambda b: b[:4] == b"RIFF" and b[8:12] == b"WAVE",
    ".glb": lambda b: b[:4] == b"glTF",
    ".usdz": lambda b: b[:4] == b"PK\x03\x04",
}


class Client:
    def __init__(self, exe, allow):
        self.p = subprocess.Popen([exe, "mcp", "--allow", allow], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.next_id = 1

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            msg["id"] = self.next_id
            self.next_id += 1
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        if notify:
            return None
        reply = json.loads(self.p.stdout.readline())
        if "error" in reply:
            raise RuntimeError("%s: %s" % (method, reply["error"]))
        return reply["result"]

    def call(self, name, args):
        r = self.send("tools/call", {"name": name, "arguments": args})
        return r["isError"], r["structuredContent"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=30)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe, keep = sys.argv[1], "--keep" in sys.argv
    work = tempfile.mkdtemp(prefix="jvconvert-selftest-")
    make_png(os.path.join(work, "gradient.png"))
    make_wav(os.path.join(work, "tone.wav"))
    make_obj(os.path.join(work, "cube.obj"))

    c = Client(exe, work)
    failures = 0
    init = c.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "selftest", "version": "1"}})
    c.send("notifications/initialized", notify=True)
    print("server:", init["serverInfo"]["name"], init["serverInfo"]["version"])
    tools = [t["name"] for t in c.send("tools/list")["tools"]]
    print("tools:", ", ".join(tools))

    jobs = [
        ({"input": "gradient.png", "output": "gradient.webp", "quality": 80}, "image"),
        ({"input": "gradient.png", "output": "gradient.jpg", "quality": 85, "max_size": 160}, "image"),
        ({"input": "tone.wav", "output": "tone.flac", "sample_rate": 48000, "bit_depth": 24}, "audio"),
        ({"input": "cube.obj", "output": "cube.glb"}, "model"),
        ({"input": "cube.glb", "output": "cube.usdz"}, "model"),
    ]
    for args, kind in jobs:
        err, res = c.call("convert", args)
        out = os.path.join(work, args["output"])
        ok = not err and res.get("ok") and os.path.isfile(out)
        if ok:
            with open(out, "rb") as f:
                ok = MAGIC[os.path.splitext(out)[1]](f.read(16))
            _, probe = c.call("probe", {"path": args["output"]})
            ok = ok and probe.get("kind") == kind
            detail = {k: probe.get(k) for k in ("format", "file_size", "width", "height",
                                                 "sample_rate", "bit_depth", "vertices", "materials")
                      if probe.get(k) is not None}
        else:
            detail = res
        print("%-4s %s -> %s %s" % ("ok" if ok else "FAIL", args["input"], args["output"], detail))
        failures += 0 if ok else 1

    err, res = c.call("probe", {"path": os.path.join(os.path.dirname(work), "outside.png")})
    denied = err and res.get("status") == "access_denied"
    print("%-4s sandbox refuses a path outside --allow" % ("ok" if denied else "FAIL"))
    failures += 0 if denied else 1
    c.close()

    print("work dir:", work if keep else "(removed)")
    if not keep:
        for name in os.listdir(work):
            os.remove(os.path.join(work, name))
        os.rmdir(work)
    print("PASS" if failures == 0 else "%d FAILED" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
