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

The events are sent with input-send-event without a device. QEMU routes
them to the input device of their type, as it routes the events of a
display window. The script exits with status 1 when a wait times out or
QEMU closes the socket.
"""
import json
import os
import re
import socket
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

    def events(self, events):
        self.call("input-send-event", {"events": events})


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
