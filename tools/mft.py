"""MFT 1, the file transfer protocol of docs/design/filetransfer.md.

The module provides the path encoding, the part files for resume, the
client and the server. tools/transfer.py builds the command line and the
window on this module. Both sides check the CRC-32 of every chunk.
"""
import collections
import errno
import os
import re
import select
import shutil
import socket
import stat
import threading
import zlib

VERSION = 1
CHUNK = 65536
PATH_MAX = 1024
LINE_MAX = 8192
PART_SUFFIX = '.mft-part'
GUEST_PORT = 9102           # the minios server, forwarded by tools/run.sh
HOST_PORT = 9103            # the host server, 10.0.2.2:9103 from the guest
IO_TIMEOUT = 120.0          # seconds without progress before a socket fails

CODES = ('ENOENT', 'EEXIST', 'EACCES', 'EINVAL', 'EIO', 'ENOSPC', 'ENOTDIR',
         'EISDIR', 'ENOTEMPTY', 'ENAMETOOLONG', 'ESTALE', 'ECANCELED', 'EPROTO')

_SAFE = frozenset(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/')
_HEX = frozenset(b'0123456789abcdefABCDEF')


class MftError(Exception):
    """A failure with a POSIX code and a message. A local failure, such as
    a refused connection, carries its own code. The wire carries only the
    codes of the protocol (wire_code)."""

    def __init__(self, code, message):
        super().__init__(f'{message} ({code})')
        self.code = code
        self.message = message
        self.errno = getattr(errno, self.code, errno.EIO)


def wire_code(code):
    """The code of an error line: a code outside the protocol is EIO."""
    return code if code in CODES else 'EIO'


def error_from_os(e):
    """Converts an OSError to an MftError with the POSIX name of its errno."""
    return MftError(errno.errorcode.get(e.errno or 0, 'EIO'), e.strerror or str(e))


Entry = collections.namedtuple('Entry', 'kind size mtime name')


# ---- paths ----

def encode_path(path):
    """Percent encodes every byte outside A-Z a-z 0-9 - . _ ~ and /."""
    data = path.encode('utf-8', 'surrogateescape')
    return ''.join(chr(c) if c in _SAFE else '%%%02X' % c for c in data)


def decode_path(text):
    """Reverses encode_path. A malformed escape is EINVAL."""
    data = text.encode('utf-8', 'surrogateescape')
    out = bytearray()
    i = 0
    while i < len(data):
        c = data[i]
        if c == 0x25:
            if i + 2 >= len(data) or data[i + 1] not in _HEX or data[i + 2] not in _HEX:
                raise MftError('EINVAL', 'malformed percent escape')
            out.append(int(data[i + 1:i + 3], 16))
            i += 3
        else:
            out.append(c)
            i += 1
    return out.decode('utf-8', 'surrogateescape')


def check_path(path):
    """Raises EINVAL for a path that the protocol rejects.

    The root folder is ".". Every other path consists of components that
    are neither empty nor "." nor "..". A path never starts with "/" and
    never contains a NUL byte."""
    if len(path.encode('utf-8', 'surrogateescape')) > PATH_MAX:
        raise MftError('ENAMETOOLONG', 'File name too long')
    if path == '.':
        return
    if not path or path.startswith('/') or '\0' in path:
        raise MftError('EINVAL', 'Invalid path')
    for part in path.split('/'):
        if part in ('', '.', '..'):
            raise MftError('EINVAL', 'Invalid path')


def join(parent, name):
    """The path of name in the folder parent."""
    return name if parent in ('.', '') else parent + '/' + name


def basename(path):
    return path.rsplit('/', 1)[-1]


def parent_of(path):
    return path.rsplit('/', 1)[0] if '/' in path else '.'


# ---- part files ----

def part_name(name, size, mtime):
    """The part file of NAME with SIZE bytes and the time MTIME."""
    return f'.{name}.{size}-{mtime}{PART_SUFFIX}'


def is_part(name):
    return name.startswith('.') and name.endswith(PART_SUFFIX)


_PART_KEY = re.compile(r'-?\d+--?\d+')


def prepare_part(folder, name, size, mtime):
    """Removes the part files of name with another size or time.

    Returns the path of the part file for size and mtime and its length.
    A part file longer than size is removed, and the length is then 0."""
    prefix = '.' + name + '.'
    current = part_name(name, size, mtime)
    try:
        names = os.listdir(folder)
    except OSError as e:
        raise error_from_os(e)
    for other in names:
        if other == current or not other.startswith(prefix) or not other.endswith(PART_SUFFIX):
            continue
        if _PART_KEY.fullmatch(other[len(prefix):-len(PART_SUFFIX)]):
            try:
                os.remove(os.path.join(folder, other))
            except OSError:
                pass
    path = os.path.join(folder, current)
    try:
        length = os.stat(path).st_size
    except FileNotFoundError:
        length = 0
    except OSError as e:
        raise error_from_os(e)
    if length > size:
        os.remove(path)
        length = 0
    return path, length


# ---- lines and chunks ----

class Channel:
    """Lines and binary data on one socket, with its own read buffer."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()
        self.failed = False             # a read or write failed, or the stream lost its framing

    def _fill(self):
        try:
            data = self.sock.recv(262144)
        except socket.timeout:
            self.fail('ETIMEDOUT', 'the connection timed out')
        except OSError as e:
            self.fail(error_from_os(e).code, f'the connection failed: {e.strerror or e}')
        if not data:
            self.fail('ECONNRESET', 'the connection was closed')
        self.buf += data

    def fail(self, code, message):
        """Marks the channel as unusable and raises the error."""
        self.failed = True
        raise MftError(code, message)

    def readline(self):
        while True:
            i = self.buf.find(b'\n')
            if i >= 0:
                line = bytes(self.buf[:i])
                del self.buf[:i + 1]
                return line.decode('utf-8', 'replace').rstrip('\r')
            if len(self.buf) > LINE_MAX:
                self.fail('EPROTO', 'the line is too long')
            self._fill()

    def read_exact(self, n):
        while len(self.buf) < n:
            self._fill()
        data = bytes(self.buf[:n])
        del self.buf[:n]
        return data

    def ready(self):
        """True when input is available without a wait."""
        if self.buf:
            return True
        r, _, _ = select.select([self.sock], [], [], 0)
        return bool(r)

    def send(self, data):
        try:
            self.sock.sendall(data)
        except socket.timeout:
            self.fail('ETIMEDOUT', 'the connection timed out')
        except OSError as e:
            self.fail(error_from_os(e).code, f'the connection failed: {e.strerror or e}')

    def send_line(self, line):
        self.send((line + '\n').encode('utf-8'))

    def send_chunk(self, data):
        self.send(b'C %d %08x\n' % (len(data), zlib.crc32(data)) + data)


def parse_chunk(line):
    """Returns the length and the CRC of the line "C LEN CRC"."""
    parts = line.split(' ')
    if len(parts) != 3 or parts[0] != 'C' or len(parts[2]) != 8:
        raise MftError('EPROTO', f'unexpected line {line[:60]!r}')
    try:
        n, crc = int(parts[1]), int(parts[2], 16)
    except ValueError:
        raise MftError('EPROTO', f'unexpected line {line[:60]!r}')
    if not 1 <= n <= CHUNK:
        raise MftError('EPROTO', f'invalid chunk length {n}')
    return n, crc


def parse_error(line):
    """The MftError of a line "ERR CODE MESSAGE" or "X CODE MESSAGE"."""
    parts = line.split(' ', 2)
    code = wire_code(parts[1]) if len(parts) > 1 else 'EIO'
    return MftError(code, parts[2] if len(parts) > 2 else code)


def _int_fields(fields, count):
    if len(fields) != count:
        raise MftError('EINVAL', 'wrong number of fields')
    try:
        return [int(f) for f in fields]
    except ValueError:
        raise MftError('EINVAL', 'a number is invalid')


def _cancelled(cancel):
    return cancel is not None and cancel.is_set()


# ---- client ----

class Job:
    """One step of a copy: a folder to create or a file to copy.

    direction is "get" or "put", kind is "d" or "f". remote and local are
    the paths on both sides. label is the path shown to the user."""

    def __init__(self, direction, kind, remote, local, size, mtime, label):
        self.direction, self.kind = direction, kind
        self.remote, self.local = remote, local
        self.size, self.mtime, self.label = size, mtime, label

    def __repr__(self):
        return f'Job({self.direction} {self.kind} {self.remote!r} {self.local!r} {self.size})'


class Client:
    """A session with an MFT server. One thread uses a client at a time."""

    def __init__(self):
        self.sock = None
        self.ch = None
        self.server_name = None

    def connect(self, host, port, timeout=10.0):
        try:
            self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        except OSError as e:
            raise error_from_os(e)
        self.sock.settimeout(IO_TIMEOUT)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.ch = Channel(self.sock)
        try:
            self.hello()
        except MftError:
            self.close()
            raise
        return self

    def hello(self):
        self.ch.send_line(f'MFT {VERSION}')
        line = self.ch.readline()
        if line.startswith('ERR '):
            raise parse_error(line)
        parts = line.split(' ')
        if len(parts) < 2 or parts[0] != 'MFT' or parts[1] != str(VERSION):
            raise MftError('EPROTO', f'not an MFT {VERSION} server')
        self.server_name = decode_path(parts[2]) if len(parts) > 2 else ''

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = self.ch = None

    @property
    def connected(self):
        """False after a failure of the connection or of its framing."""
        return self.ch is not None and not self.ch.failed

    def _call(self, line):
        """Sends a request and returns the fields of the OK reply."""
        if not self.connected:
            raise MftError('EIO', 'not connected')
        self.ch.send_line(line)
        reply = self.ch.readline()
        if reply.startswith('ERR'):
            raise parse_error(reply)
        if reply != 'OK' and not reply.startswith('OK '):
            self.ch.fail('EPROTO', f'unexpected reply {reply[:60]!r}')
        return reply.split(' ')[1:]

    def list(self, path='.'):
        count, = _int_fields(self._call(f'LIST {encode_path(path)}'), 1)
        entries = []
        for _ in range(count):
            fields = self.ch.readline().split(' ')
            if len(fields) != 4 or fields[0] not in ('d', 'f'):
                self.ch.fail('EPROTO', 'malformed entry line')
            size, mtime = _int_fields(fields[1:3], 2)
            entries.append(Entry(fields[0], size, mtime, decode_path(fields[3])))
        return entries

    def stat(self, path):
        fields = self._call(f'STAT {encode_path(path)}')
        if len(fields) != 3 or fields[0] not in ('d', 'f'):
            self.ch.fail('EPROTO', 'malformed STAT reply')
        size, mtime = _int_fields(fields[1:], 2)
        return Entry(fields[0], size, mtime, basename(path))

    def mkdir(self, path):
        self._call(f'MKDIR {encode_path(path)}')

    def rename(self, old, new):
        self._call(f'RENAME {encode_path(old)} {encode_path(new)}')

    def delete(self, path):
        self._call(f'DELETE {encode_path(path)}')

    def quit(self):
        try:
            if self.ch:
                self._call('QUIT')
        finally:
            self.close()

    def get(self, path, local_dir, size, mtime, progress=None, cancel=None):
        """Copies the remote file path into local_dir.

        The copy resumes from the part file in local_dir. progress is
        called with the bytes present and the size. Returns the bytes
        received over the connection."""
        name = basename(path)
        part, offset = prepare_part(local_dir, name, size, mtime)
        self._call(f'GET {encode_path(path)} {offset} {size} {mtime}')
        done, received = offset, 0
        bad = failed = None
        cancelled = False
        if progress:
            progress(done, size)
        try:
            f = open(part, 'ab')
        except OSError as e:
            f, failed = None, error_from_os(e)
            self.ch.send_line('A')
            cancelled = True
        try:
            while True:
                if _cancelled(cancel) and not cancelled:
                    self.ch.send_line('A')
                    cancelled = True
                line = self.ch.readline()
                if line in ('E', 'A'):
                    break
                if line.startswith('X '):
                    failed = failed or parse_error(line)
                    break
                try:
                    n, crc = parse_chunk(line)
                except MftError as e:
                    self.ch.fail(e.code, e.message)
                data = self.ch.read_exact(n)
                if cancelled or bad or failed:
                    continue
                if zlib.crc32(data) != crc:
                    bad = MftError('EIO', 'checksum mismatch')
                    continue
                if done + n > size:
                    bad = MftError('EPROTO', 'more data than the size of the file')
                    continue
                try:
                    f.write(data)
                except OSError as e:
                    failed = error_from_os(e)
                    self.ch.send_line('A')
                    cancelled = True
                    continue
                done += n
                received += n
                if progress:
                    progress(done, size)
        finally:
            if f:
                f.close()
        if failed:
            raise failed
        if cancelled:
            raise MftError('ECANCELED', 'the transfer was cancelled')
        if bad:
            raise bad
        if done != size:
            raise MftError('EIO', f'the data ended after {done} of {size} bytes')
        try:
            os.utime(part, (mtime, mtime))
            os.replace(part, os.path.join(local_dir, name))
        except OSError as e:
            raise error_from_os(e)
        return received

    def put(self, local_path, remote_path, progress=None, cancel=None):
        """Copies the local file to the remote file path.

        The copy resumes from the length that the server reports. Returns
        the bytes sent over the connection."""
        try:
            f = open(local_path, 'rb')
        except OSError as e:
            raise error_from_os(e)
        with f:
            st = os.fstat(f.fileno())
            size, mtime = st.st_size, int(st.st_mtime)
            offset, = _int_fields(self._call(f'PUT {encode_path(remote_path)} {size} {mtime}'), 1)
            if not 0 <= offset <= size:
                self.ch.fail('EPROTO', f'invalid offset {offset}')
            done, sent = offset, 0
            if progress:
                progress(done, size)
            failure = None
            try:
                f.seek(offset)
            except OSError as e:
                failure = error_from_os(e)
            while done < size and failure is None:
                if _cancelled(cancel):
                    failure = MftError('ECANCELED', 'the transfer was cancelled')
                    break
                try:
                    data = f.read(min(CHUNK, size - done))
                except OSError as e:
                    failure = error_from_os(e)
                    break
                if not data:
                    break
                self.ch.send_chunk(data)
                done += len(data)
                sent += len(data)
                if progress:
                    progress(done, size)
            if failure is not None:
                self.ch.send_line('A')
                reply = self.ch.readline()
                if not reply.startswith('ERR'):
                    self.ch.fail('EPROTO', f'unexpected reply {reply[:60]!r}')
                raise failure
            self.ch.send_line('E')
            reply = self.ch.readline()
            if reply.startswith('ERR'):
                raise parse_error(reply)
            if reply != 'OK':
                self.ch.fail('EPROTO', f'unexpected reply {reply[:60]!r}')
            return sent

    # ---- copies of trees ----

    def plan_get(self, path, kind, size, mtime, local_dir, label=None):
        """The jobs that copy the remote file or folder into local_dir."""
        name = basename(path)
        label = label or name
        local = os.path.join(local_dir, name)
        if kind != 'd':
            return [Job('get', 'f', path, local, size, mtime, label)]
        jobs = [Job('get', 'd', path, local, 0, mtime, label)]
        for e in sorted(self.list(path), key=lambda e: e.name):
            jobs += self.plan_get(join(path, e.name), e.kind, e.size, e.mtime, local, label + '/' + e.name)
        return jobs

    def plan_put(self, local_path, remote_dir, label=None):
        """The jobs that copy the local file or folder into remote_dir."""
        local_path = os.path.normpath(local_path)
        name = os.path.basename(local_path)
        label = label or name
        remote = join(remote_dir, name)
        try:
            st = os.stat(local_path)
        except OSError as e:
            raise error_from_os(e)
        if stat.S_ISREG(st.st_mode):
            return [Job('put', 'f', remote, local_path, st.st_size, int(st.st_mtime), label)]
        if not stat.S_ISDIR(st.st_mode):
            raise MftError('EINVAL', f'{local_path} is not a file or folder')
        jobs = [Job('put', 'd', remote, local_path, 0, int(st.st_mtime), label)]
        for e in local_entries(local_path):
            jobs += self.plan_put(os.path.join(local_path, e.name), remote, label + '/' + e.name)
        return jobs

    def run(self, job, progress=None, cancel=None):
        """Performs one job. Returns the bytes moved over the connection."""
        if job.kind == 'd':
            if job.direction == 'get':
                try:
                    os.makedirs(job.local, exist_ok=True)
                except OSError as e:
                    raise error_from_os(e)
            else:
                try:
                    self.mkdir(job.remote)
                except MftError as e:
                    if e.code != 'EEXIST':
                        raise
            return 0
        if job.direction == 'get':
            return self.get(job.remote, os.path.dirname(job.local), job.size, job.mtime, progress, cancel)
        return self.put(job.local, job.remote, progress, cancel)

    def get_tree(self, path, local_dir, progress=None, cancel=None):
        """Copies the remote file or folder path into local_dir."""
        e = self.stat(path)
        total = 0
        for job in self.plan_get(path, e.kind, e.size, e.mtime, local_dir):
            total += self.run(job, progress, cancel)
        return total

    def put_tree(self, local_path, remote_dir, progress=None, cancel=None):
        """Copies the local file or folder into the remote folder."""
        total = 0
        for job in self.plan_put(local_path, remote_dir):
            total += self.run(job, progress, cancel)
        return total


def local_entries(folder):
    """The folders and regular files of a local folder as entries.

    Symbolic links are followed. Part files and other kinds of files are
    omitted. The order is the order of the names."""
    try:
        names = sorted(os.listdir(folder))
    except OSError as e:
        raise error_from_os(e)
    entries = []
    for name in names:
        if is_part(name):
            continue
        try:
            st = os.stat(os.path.join(folder, name))
        except OSError:
            continue
        if stat.S_ISDIR(st.st_mode):
            entries.append(Entry('d', 0, int(st.st_mtime), name))
        elif stat.S_ISREG(st.st_mode):
            entries.append(Entry('f', st.st_size, int(st.st_mtime), name))
    return entries


# ---- server ----

class Server:
    """Serves the folder root on a TCP port. Each session has a thread.

    log, when given, receives one line per completed transfer:
    "stored PATH, SIZE bytes" and "sent PATH, SIZE bytes", followed by
    ", resumed at OFFSET" for a resumed transfer."""

    def __init__(self, root, port=HOST_PORT, address='0.0.0.0', log=None):
        self.root = os.path.abspath(root)
        self.address, self.port = address, int(port)
        self.log = log
        self.listener = None
        self.thread = None
        self.stopping = threading.Event()
        self.lock = threading.Lock()        # protects sessions
        self.sessions = {}                  # socket -> thread

    @property
    def clients(self):
        with self.lock:
            return len(self.sessions)

    def start(self):
        if not os.path.isdir(self.root):
            raise MftError('ENOTDIR', f'{self.root} is not a folder')
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((self.address, self.port))
            s.listen(16)
        except OSError as e:
            s.close()
            raise error_from_os(e)
        s.settimeout(0.2)
        self.listener = s
        self.port = s.getsockname()[1]
        self.thread = threading.Thread(target=self._accept_loop, name='mft-accept', daemon=True)
        self.thread.start()
        return self

    def stop(self):
        """Closes the listening socket and every session, then waits for
        the threads."""
        self.stopping.set()
        if self.thread:
            self.thread.join()
            self.thread = None
        with self.lock:
            sessions = list(self.sessions.items())
        for sock, _ in sessions:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        for _, t in sessions:
            t.join()

    def _accept_loop(self):
        try:
            while not self.stopping.is_set():
                try:
                    sock, _ = self.listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                sock.settimeout(IO_TIMEOUT)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                t = threading.Thread(target=self._session, args=(sock,), name='mft-session', daemon=True)
                with self.lock:
                    self.sessions[sock] = t
                t.start()
        finally:
            self.listener.close()

    def _log(self, line):
        if self.log:
            self.log(line)

    def _local(self, text):
        """The local path of an encoded protocol path."""
        path = decode_path(text)
        check_path(path)
        if path == '.':
            return path, self.root
        return path, os.path.join(self.root, *path.split('/'))

    def _session(self, sock):
        ch = Channel(sock)
        try:
            line = ch.readline()
            if line != f'MFT {VERSION}':
                ch.send_line('ERR EPROTO unsupported version')
                return
            ch.send_line(f'MFT {VERSION} {encode_path(os.path.basename(self.root) or self.root)}')
            while not self.stopping.is_set():
                # The wait for the next request has no limit. stop() ends
                # it with a shutdown of the socket.
                sock.settimeout(None)
                line = ch.readline()
                sock.settimeout(IO_TIMEOUT)
                fields = line.split(' ')
                request, args = fields[0], fields[1:]
                if request == 'A':
                    continue
                if request == 'QUIT':
                    ch.send_line('OK')
                    return
                handler = self._handlers.get(request)
                try:
                    if handler is None:
                        raise _Reply(MftError('EINVAL', f'unknown request {request[:20]}'))
                    handler(self, ch, args)
                except OSError as e:
                    ch.send_line(f'ERR {wire_code(error_from_os(e).code)} {e.strerror or e}')
                except _Reply as r:
                    ch.send_line(f'ERR {wire_code(r.error.code)} {r.error.message}')
        except (MftError, OSError):
            pass
        finally:
            with self.lock:
                self.sessions.pop(sock, None)
            try:
                sock.close()
            except OSError:
                pass

    # The handlers reply OK themselves. A handler raises _Reply for an
    # error reply that leaves the session usable. Any other MftError ends
    # the session.

    def _list(self, ch, args):
        path, local = self._args_paths(args, 1)[0]
        if not os.path.isdir(local):
            if os.path.exists(local):
                raise _Reply(MftError('ENOTDIR', 'Not a directory'))
            raise _Reply(MftError('ENOENT', 'No such file or directory'))
        try:
            entries = local_entries(local)
        except MftError as e:
            raise _Reply(e)
        lines = [f'OK {len(entries)}']
        lines += [f'{e.kind} {e.size} {e.mtime} {encode_path(e.name)}' for e in entries]
        ch.send(('\n'.join(lines) + '\n').encode('utf-8'))

    def _stat(self, ch, args):
        path, local = self._args_paths(args, 1)[0]
        st = os.stat(local)
        if stat.S_ISDIR(st.st_mode):
            ch.send_line(f'OK d 0 {int(st.st_mtime)}')
        elif stat.S_ISREG(st.st_mode):
            ch.send_line(f'OK f {st.st_size} {int(st.st_mtime)}')
        else:
            raise _Reply(MftError('EINVAL', 'Not a file or directory'))

    def _get(self, ch, args):
        if len(args) != 4:
            raise _Reply(MftError('EINVAL', 'GET needs a path, an offset, a size and a time'))
        path, local = self._path(args[0])
        offset, size, mtime = self._ints(args[1:])
        f = open(local, 'rb')
        with f:
            st = os.fstat(f.fileno())
            if stat.S_ISDIR(st.st_mode):
                raise _Reply(MftError('EISDIR', 'Is a directory'))
            if st.st_size != size or int(st.st_mtime) != mtime:
                raise _Reply(MftError('ESTALE', 'The file has changed'))
            if offset > size:
                raise _Reply(MftError('EINVAL', 'the offset is beyond the end of the file'))
            ch.send_line('OK')
            f.seek(offset)
            done = offset
            while done < size:
                if ch.ready():
                    line = ch.readline()
                    if line != 'A':
                        raise MftError('EPROTO', 'unexpected input during a transfer')
                    ch.send_line('A')
                    return
                try:
                    data = f.read(min(CHUNK, size - done))
                except OSError as e:
                    ch.send_line(f'X {wire_code(error_from_os(e).code)} {e.strerror or e}')
                    return
                if not data:
                    ch.send_line('X EIO the file became shorter')
                    return
                ch.send_chunk(data)
                done += len(data)
            ch.send_line('E')
        self._log(f'sent {path}, {size} bytes' + (f', resumed at {offset}' if offset else ''))

    def _put(self, ch, args):
        if len(args) != 3:
            raise _Reply(MftError('EINVAL', 'PUT needs a path, a size and a time'))
        path, local = self._path(args[0])
        size, mtime = self._ints(args[1:])
        if path == '.' or size < 0:
            raise _Reply(MftError('EINVAL', 'Invalid argument'))
        folder, name = os.path.split(local)
        if not os.path.isdir(folder):
            raise _Reply(MftError('ENOENT', 'No such file or directory'))
        if os.path.isdir(local):
            raise _Reply(MftError('EISDIR', 'Is a directory'))
        try:
            part, offset = prepare_part(folder, name, size, mtime)
            f = open(part, 'ab')
        except MftError as e:
            raise _Reply(e)
        ch.send_line(f'OK {offset}')
        written = offset
        failure = None
        cancelled = False
        with f:
            while True:
                line = ch.readline()
                if line == 'E':
                    break
                if line == 'A':
                    cancelled = True
                    break
                n, crc = parse_chunk(line)
                data = ch.read_exact(n)
                if failure:
                    continue
                if zlib.crc32(data) != crc:
                    failure = MftError('EIO', 'checksum mismatch')
                    continue
                if written + n > size:
                    failure = MftError('EINVAL', 'more data than the size of the file')
                    continue
                try:
                    f.write(data)
                except OSError as e:
                    failure = error_from_os(e)
                    continue
                written += n
        if cancelled:
            raise _Reply(MftError('ECANCELED', 'the transfer was cancelled'))
        if failure:
            raise _Reply(failure)
        if written != size:
            raise _Reply(MftError('EIO', f'the data ended after {written} of {size} bytes'))
        os.utime(part, (mtime, mtime))
        os.replace(part, local)
        ch.send_line('OK')
        self._log(f'stored {path}, {size} bytes' + (f', resumed at {offset}' if offset else ''))

    def _mkdir(self, ch, args):
        path, local = self._args_paths(args, 1)[0]
        if path == '.':
            raise _Reply(MftError('EEXIST', 'the root folder exists'))
        os.mkdir(local)
        ch.send_line('OK')

    def _rename(self, ch, args):
        (old, old_local), (new, new_local) = self._args_paths(args, 2)
        if '.' in (old, new):
            raise _Reply(MftError('EINVAL', 'the root folder cannot be renamed'))
        if not os.path.lexists(old_local):
            raise _Reply(MftError('ENOENT', 'No such file or directory'))
        if os.path.lexists(new_local):
            raise _Reply(MftError('EEXIST', 'File exists'))
        os.rename(old_local, new_local)
        ch.send_line('OK')

    def _delete(self, ch, args):
        path, local = self._args_paths(args, 1)[0]
        if path == '.':
            raise _Reply(MftError('EINVAL', 'the root folder cannot be removed'))
        if os.path.isdir(local) and not os.path.islink(local):
            shutil.rmtree(local)
        else:
            os.remove(local)
        ch.send_line('OK')

    def _path(self, text):
        try:
            return self._local(text)
        except MftError as e:
            raise _Reply(e)

    def _args_paths(self, args, count):
        if len(args) != count:
            raise _Reply(MftError('EINVAL', 'wrong number of fields'))
        return [self._path(a) for a in args]

    @staticmethod
    def _ints(fields):
        try:
            return _int_fields(fields, len(fields))
        except MftError as e:
            raise _Reply(e)

    _handlers = {
        'LIST': _list, 'STAT': _stat, 'GET': _get, 'PUT': _put,
        'MKDIR': _mkdir, 'RENAME': _rename, 'DELETE': _delete,
    }


class _Reply(Exception):
    """An error reply of a request. The session continues."""

    def __init__(self, error):
        super().__init__(error.message)
        self.error = error
