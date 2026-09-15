#!/usr/bin/env python3
"""Host side of xfer(1), the same protocol.
   xfer.py serve [-p PORT] [DIR]          serve DIR (default .)
   xfer.py put  [-h HOST] [-p PORT] PATH...
   xfer.py get  [-h HOST] [-p PORT] [-o DIR] NAME...
   xfer.py ls   [-h HOST] [-p PORT] [DIR]
HOST defaults to 127.0.0.1 (a guest reached through a QEMU port forward,
which make run --nic user sets up on port 9100), PORT to 9100."""
import os, socket, sys, zlib, threading, time

HOST, PORT, OUT = '127.0.0.1', 9100, '.'
CHUNK = 65536


def safe(p):
    return p and not p.startswith('/') and all(c not in ('', '.', '..') for c in p.split('/'))


def read_line(s):
    line = b''
    while not line.endswith(b'\n'):
        c = s.recv(1)
        if not c:
            break
        line += c
    return line.decode(errors='replace').strip()


class Progress:
    """Once per second, when standard error is a terminal, one line with the
    bytes done and the throughput, overwritten in place."""

    def __init__(self, label, total):
        self.label, self.total, self.done = label, total, 0
        self.start = self.shown = time.time()
        self.tty = sys.stderr.isatty()

    def add(self, n):
        self.done += n
        now = time.time()
        if self.tty and now - self.shown >= 1:
            self.shown = now
            rate = self.done / 1024 / (now - self.start + 1e-3)
            sys.stderr.write(f'\r{self.label}: {self.done} of {self.total} bytes, {rate:.0f} KiB/s')
            sys.stderr.flush()

    def end(self):
        if self.tty and self.shown != self.start:
            sys.stderr.write('\n')
            sys.stderr.flush()


def recv_to(s, n, f, label):
    crc = 0
    progress = Progress(label, n)
    while n:
        chunk = s.recv(min(n, CHUNK))
        if not chunk:
            progress.end()
            raise EOFError('connection closed')
        f.write(chunk)
        crc = zlib.crc32(chunk, crc)
        n -= len(chunk)
        progress.add(len(chunk))
    progress.end()
    return crc & 0xffffffff


def file_crc(path):
    crc = 0
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(CHUNK), b''):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xffffffff


def send_file(s, path, label):
    progress = Progress(label, os.path.getsize(path))
    try:
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(CHUNK), b''):
                s.sendall(chunk)
                progress.add(len(chunk))
    finally:
        progress.end()


def log(msg):
    print('xfer: ' + msg, flush=True)


def handle(c, directory):
    with c:
        c.settimeout(30)
        words = read_line(c).split(None, 2)
        try:
            if len(words) == 3 and words[0] == 'PUT' and safe(words[2]):
                name, size = words[2], words[1]
                path = os.path.join(directory, name)
                os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
                started = time.time()
                with open(path, 'wb') as f:
                    crc = recv_to(c, int(size), f, name)
                c.sendall(f'OK {size} {crc:08x}\n'.encode())
                log(f'stored {name}, {size} bytes, {int(size) / 1024 / (time.time() - started + 1e-3):.0f} KiB/s')
            elif len(words) >= 2 and words[0] == 'GET' and safe(' '.join(words[1:])):
                words = ['GET', ' '.join(words[1:])]
                path = os.path.join(directory, words[1])
                if os.path.isdir(path):
                    c.sendall(b'ERR is a directory\n')
                    return
                size = os.path.getsize(path)
                c.sendall(f'OK {size} {file_crc(path):08x}\n'.encode())
                send_file(c, path, words[1])
                log(f'served {words[1]}, {size} bytes')
            elif words and words[0] == 'LIST' and (len(words) == 1 or ' '.join(words[1:]) == '.' or safe(' '.join(words[1:]))):
                path = directory if len(words) == 1 or ' '.join(words[1:]) == '.' else os.path.join(directory, ' '.join(words[1:]))
                names = sorted(os.listdir(path))
                c.sendall(b'OK\n')
                for n in names:
                    p = os.path.join(path, n)
                    if os.path.isdir(p):
                        c.sendall(f'D {n}\n'.encode())
                    else:
                        c.sendall(f'F {os.path.getsize(p)} {file_crc(p):08x} {n}\n'.encode())
                c.sendall(b'END\n')
            else:
                c.sendall(b'ERR bad request\n')
        except OSError as e:
            try:
                c.sendall(f'ERR {e.strerror or e}\n'.encode())
            except OSError:
                pass


def serve(directory):
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', PORT))
    server.listen(8)
    log(f'serving {directory} on port {PORT}')
    while True:
        c, _ = server.accept()
        threading.Thread(target=handle, args=(c, directory), daemon=True).start()


def connect():
    try:
        return socket.create_connection((HOST, PORT), timeout=30)
    except OSError as e:
        sys.exit(f'xfer: connect {HOST}:{PORT}: {e.strerror or e}'
                 + ('; is the guest running "xfer serve" behind the port forward?' if PORT == 9100 else ''))


def put_file(path, rel):
    size = os.path.getsize(path)
    with connect() as s:
        s.sendall(f'PUT {size} {rel}\n'.encode())
        try:
            send_file(s, path, rel)
        except BrokenPipeError:
            log(f'{rel}: the server closed the connection')
            return 1
        reply = read_line(s).split()
    ok = len(reply) == 3 and reply[0] == 'OK' and int(reply[2], 16) == file_crc(path)
    log(f'{"sent" if ok else "failed"} {rel}, {size} bytes' + ('' if ok else ': ' + ' '.join(reply)))
    return 0 if ok else 1


def put(path, rel=None):
    rel = rel or os.path.basename(path.rstrip('/'))
    if os.path.isdir(path):
        return max([put(os.path.join(path, n), rel + '/' + n) for n in sorted(os.listdir(path))] or [0])
    return put_file(path, rel)


def listing(rel):
    with connect() as s:
        s.sendall(f'LIST {rel}\n'.encode())
        if not read_line(s).startswith('OK'):
            return None
        rows = []
        while True:
            line = read_line(s)
            if line == 'END' or not line:
                return rows
            rows.append(line.split(None, 3))


def get_file(rel):
    with connect() as s:
        s.sendall(f'GET {rel}\n'.encode())
        reply = read_line(s).split()
        if len(reply) != 3 or reply[0] != 'OK':
            log(f'{rel}: {" ".join(reply)}')
            return 1
        path = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
        with open(path, 'wb') as f:
            crc = recv_to(s, int(reply[1]), f, rel)
    if crc != int(reply[2], 16):
        os.unlink(path)
        log(f'{rel}: checksum mismatch, the copy is damaged')
        return 1
    log(f'received {rel}, {reply[1]} bytes')
    return 0


def get(rel):
    rows = listing(rel)
    if rows is None:
        return get_file(rel)
    return max([get(rel + '/' + (r[1] if r[0] == 'D' else r[3])) for r in rows] or [0])


def ls(rel):
    rows = listing(rel)
    if rows is None:
        log(f'{rel}: cannot list')
        return 1
    for r in rows:
        print(f'{"":>12}  {r[1]}/' if r[0] == 'D' else f'{r[1]:>12}  {r[3]}')
    return 0


args = sys.argv[1:]
if not args:
    sys.exit(__doc__)
cmd, args = args[0], args[1:]
while args and args[0].startswith('-') and len(args[0]) > 1:
    flag, value = args[0], args[1]
    if flag == '-h': HOST = value
    elif flag == '-p': PORT = int(value)
    elif flag == '-o': OUT = value
    else: sys.exit(__doc__)
    args = args[2:]
if cmd == 'serve':
    serve(args[0] if args else '.')
elif cmd == 'put' and args:
    sys.exit(max(put(p) for p in args))
elif cmd == 'get' and args:
    sys.exit(max(get(p) for p in args))
elif cmd == 'ls':
    sys.exit(ls(args[0] if args else '.'))
else:
    sys.exit(__doc__)
