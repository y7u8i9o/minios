#!/usr/bin/env python3
"""Send input events to a running QEMU through QMP, for the boot tests.

Usage: qmp_input.py SOCKET SERIAL SCRIPT

SOCKET is the path of the QMP socket of QEMU, SERIAL the serial log of the
guest and SCRIPT a file with one command per line:

    wait REGEX      wait until a line of the serial log matches REGEX
    key NAME        press and release the key with the QEMU key code NAME
    abs X Y         move the absolute pointer to X and Y (0 to 32767)
    button NAME     press and release the button NAME (left, right, middle)
    rel DX DY       move the relative pointer
    sleep MS        wait MS milliseconds
    screendump FILE save the screen as a PPM image; a relative FILE is
                    placed in the directory of the serial log
    qmp CMD [JSON]  run the QMP command CMD with the arguments JSON and
                    print its result as one line of JSON
    event NAME      wait until QEMU sends the event NAME and print its data
                    as one line of JSON
    vnc-size W H    ask for a display of W by H pixels with the message
                    SetDesktopSize of a VNC client; the case needs a vnc
                    file, which sets VNC_SOCKET

The events are sent with input-send-event without a device. QEMU routes
them to the input device of their type, as it routes the events of a
display window. The script exits with status 1 when a wait times out or
QEMU closes the socket.
"""
import json
import os
import re
import socket
import struct
import sys
import time

WAIT_SECONDS = 60


def connect(path):
    deadline = time.time() + 20
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


class Qmp:
    def __init__(self, path):
        self.sock = connect(path)
        self.file = self.sock.makefile("rw")
        self.events_seen = []                   # events read while waiting for replies
        self.file.readline()                    # the greeting
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        msg = {"execute": command}
        if arguments is not None:
            msg["arguments"] = arguments
        self.file.write(json.dumps(msg) + "\n")
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise EOFError("QEMU closed the QMP socket")
            reply = json.loads(line)
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(f"{command}: {reply['error']}")
            # An asynchronous event; the reply follows.
            if "event" in reply:
                self.events_seen.append(reply)

    def wait_event(self, name):
        for i, e in enumerate(self.events_seen):
            if e["event"] == name:
                del self.events_seen[: i + 1]
                return e
        deadline = time.time() + WAIT_SECONDS
        self.sock.settimeout(WAIT_SECONDS)
        while time.time() < deadline:
            line = self.file.readline()
            if not line:
                raise EOFError(f"QEMU closed the QMP socket before the event {name}")
            reply = json.loads(line)
            if reply.get("event") == name:
                return reply
        raise TimeoutError(f"no event {name} after {WAIT_SECONDS} s")

    def events(self, events):
        self.call("input-send-event", {"events": events})


class Vnc:
    """A VNC client without a framebuffer: RFB 3.8 without security. The
    client sends the encoding ExtendedDesktopSize in its list and only sends
    SetDesktopSize. It never asks for a framebuffer update."""

    EXTENDED_DESKTOP_SIZE = -308

    def __init__(self, path):
        self.sock = connect(path)
        self.sock.settimeout(WAIT_SECONDS)
        version = self.read(12)
        if not version.startswith(b"RFB "):
            raise ValueError(f"not a VNC server: {version!r}")
        self.sock.sendall(b"RFB 003.008\n")
        types = self.read(self.read(1)[0])
        if 1 not in types:
            raise ValueError(f"the VNC server requires security: types {list(types)}")
        self.sock.sendall(bytes([1]))
        if struct.unpack(">I", self.read(4))[0] != 0:
            raise ValueError("the VNC server refused the connection")
        self.sock.sendall(bytes([1]))                   # ClientInit, shared
        width, height = struct.unpack(">HH", self.read(4))
        self.read(16)                                   # the pixel format
        self.read(struct.unpack(">I", self.read(4))[0])  # the desktop name
        print(f"vnc: display {width}x{height}", flush=True)
        self.sock.sendall(struct.pack(">BBHi", 2, 0, 1, self.EXTENDED_DESKTOP_SIZE))

    def read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise EOFError("QEMU closed the VNC socket")
            data += chunk
        return data

    # The status codes of ExtendedDesktopSize. QEMU answers 4 when it passed
    # the request to the display device.
    STATUS = {0: "no error", 1: "resize is administratively prohibited", 2: "out of resources",
              3: "invalid screen layout", 4: "request forwarded"}

    def set_size(self, width, height):
        """Sends SetDesktopSize and returns the status of the answer."""
        screen = struct.pack(">IHHHHI", 0, 0, 0, width, height, 0)
        self.sock.sendall(struct.pack(">BBHHBB", 251, 0, width, height, 1, 0) + screen)
        # The answer is a framebuffer update with one ExtendedDesktopSize
        # rectangle. Its x is the reason (1, a request of this client) and its
        # y the status. Other messages before it are skipped.
        while True:
            kind = self.read(1)[0]
            if kind == 0:
                rects = struct.unpack(">xH", self.read(3))[0]
                for _ in range(rects):
                    x, y, w, h, encoding = struct.unpack(">HHHHi", self.read(12))
                    if encoding != self.EXTENDED_DESKTOP_SIZE:
                        raise ValueError(f"unexpected rectangle with encoding {encoding}")
                    screens = self.read(4)[0]
                    self.read(16 * screens)
                    if x != 1:
                        continue
                    text = f"status {y} ({self.STATUS.get(y, 'unknown')}), display {w}x{h}"
                    if y not in (0, 4):
                        raise ValueError(f"SetDesktopSize {width}x{height} refused: {text}")
                    return text
            elif kind == 2:
                pass                                    # Bell
            elif kind == 3:                             # ServerCutText
                self.read(struct.unpack(">xxxI", self.read(7))[0])
            else:
                raise ValueError(f"unexpected VNC message {kind}")


def wait_serial(path, pattern):
    regex = re.compile(pattern)
    deadline = time.time() + WAIT_SECONDS
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                for line in f.read().decode("utf-8", "replace").splitlines():
                    if regex.search(line):
                        return
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"no line matches /{pattern}/ after {WAIT_SECONDS} s")


def run(qmp, serial, script):
    vnc = None
    for raw in script:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        args = rest.split()
        print(line, flush=True)
        if word == "wait":
            wait_serial(serial, rest)
        elif word == "key":
            key = {"type": "qcode", "data": args[0]}
            qmp.events([{"type": "key", "data": {"down": True, "key": key}}])
            time.sleep(0.05)
            qmp.events([{"type": "key", "data": {"down": False, "key": key}}])
        elif word == "abs":
            qmp.events([{"type": "abs", "data": {"axis": "x", "value": int(args[0])}},
                        {"type": "abs", "data": {"axis": "y", "value": int(args[1])}}])
        elif word == "button":
            qmp.events([{"type": "btn", "data": {"down": True, "button": args[0]}}])
            time.sleep(0.05)
            qmp.events([{"type": "btn", "data": {"down": False, "button": args[0]}}])
        elif word == "rel":
            qmp.events([{"type": "rel", "data": {"axis": "x", "value": int(args[0])}},
                        {"type": "rel", "data": {"axis": "y", "value": int(args[1])}}])
        elif word == "sleep":
            time.sleep(int(args[0]) / 1000)
        elif word == "qmp":
            command, _, arguments = rest.partition(" ")
            result = qmp.call(command, json.loads(arguments) if arguments.strip() else None)
            print(json.dumps(result, sort_keys=True), flush=True)
        elif word == "event":
            e = qmp.wait_event(args[0])
            print(json.dumps(e.get("data", {}), sort_keys=True), flush=True)
        elif word == "vnc-size":
            if vnc is None:
                if not os.environ.get("VNC_SOCKET"):
                    raise ValueError("vnc-size needs a vnc file in the case")
                vnc = Vnc(os.environ["VNC_SOCKET"])
            print(f"vnc: {vnc.set_size(int(args[0]), int(args[1]))}", flush=True)
        elif word == "screendump":
            path = args[0] if os.path.isabs(args[0]) else os.path.join(os.path.dirname(os.path.abspath(serial)), args[0])
            qmp.call("screendump", {"filename": path})
        else:
            raise ValueError(f"unknown command {word}")


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    sock, serial, script_path = sys.argv[1:]
    with open(script_path) as f:
        script = f.readlines()
    try:
        run(Qmp(sock), serial, script)
    except (OSError, EOFError, RuntimeError, TimeoutError, ValueError) as e:
        print(f"qmp_input: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
