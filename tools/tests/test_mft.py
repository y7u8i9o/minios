"""Unit tests of tools/mft.py and of the window of tools/transfer.py.

    python3 -m unittest discover -s tools/tests -p 'test_mft.py' -v
"""
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest
import zlib

# No __pycache__ folders in the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mft  # noqa: E402
import transfer  # noqa: E402


def write(path, data, mtime=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    if mtime is not None:
        os.utime(path, (mtime, mtime))


def read(path):
    with open(path, 'rb') as f:
        return f.read()


class Raw:
    """A raw protocol session for the tests of malformed input."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.ch = mft.Channel(self.sock)
        self.ch.send_line('MFT 1')
        assert self.ch.readline().startswith('MFT 1')

    def call(self, line):
        self.ch.send_line(line)
        return self.ch.readline()

    def close(self):
        self.sock.close()


class Paths(unittest.TestCase):
    def test_round_trip(self):
        for p in ('a', 'big name with spaces.wav', 'dir/sub/x%y', 'ü/日本', 'a+b&c=d?#'):
            enc = mft.encode_path(p)
            self.assertNotIn(' ', enc)
            self.assertEqual(mft.decode_path(enc), p)
        self.assertEqual(mft.encode_path('a b/c'), 'a%20b/c')
        self.assertEqual(mft.decode_path('a%2fb'), 'a/b')

    def test_bad_escapes(self):
        for text in ('%', '%2', '%zz', 'a%g0'):
            with self.assertRaises(mft.MftError) as cm:
                mft.decode_path(text)
            self.assertEqual(cm.exception.code, 'EINVAL')

    def test_unsafe_paths(self):
        mft.check_path('.')
        mft.check_path('a/b c/d')
        for p in ('', '/', '/etc', '..', 'a/..', 'a/./b', './a', 'a//b', 'a/', 'a\0b'):
            with self.assertRaises(mft.MftError, msg=p) as cm:
                mft.check_path(p)
            self.assertEqual(cm.exception.code, 'EINVAL')
        with self.assertRaises(mft.MftError) as cm:
            mft.check_path('x' * 1025)
        self.assertEqual(cm.exception.code, 'ENAMETOOLONG')

    def test_part_files(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, '.a.10-5.mft-part'), b'12345')
            write(os.path.join(d, '.a.20-5.mft-part'), b'1')
            write(os.path.join(d, '.a.b.10-5.mft-part'), b'1')
            path, length = mft.prepare_part(d, 'a', 10, 5)
            self.assertEqual(length, 5)
            self.assertEqual(os.path.basename(path), '.a.10-5.mft-part')
            self.assertFalse(os.path.exists(os.path.join(d, '.a.20-5.mft-part')))
            self.assertTrue(os.path.exists(os.path.join(d, '.a.b.10-5.mft-part')))
            write(os.path.join(d, '.c.3-1.mft-part'), b'too long')
            path, length = mft.prepare_part(d, 'c', 3, 1)
            self.assertEqual(length, 0)
            self.assertFalse(os.path.exists(path))


class ServerCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='mft-test-')
        self.root = os.path.join(self.tmp, 'served')
        self.local = os.path.join(self.tmp, 'local')
        os.makedirs(self.root)
        os.makedirs(self.local)
        self.log = []
        self.server = mft.Server(self.root, 0, '127.0.0.1', log=self.log.append).start()
        self.client = mft.Client().connect('127.0.0.1', self.server.port)

    def tearDown(self):
        self.client.close()
        self.server.stop()
        shutil.rmtree(self.tmp)

    def reconnect(self):
        self.client.close()
        self.client = mft.Client().connect('127.0.0.1', self.server.port)


class Requests(ServerCase):
    def test_hello(self):
        self.assertEqual(self.client.server_name, 'served')
        raw = socket.create_connection(('127.0.0.1', self.server.port), timeout=10)
        ch = mft.Channel(raw)
        ch.send_line('MFT 2')
        self.assertEqual(ch.readline(), 'ERR EPROTO unsupported version')
        raw.close()

    def test_list_and_stat(self):
        write(os.path.join(self.root, 'b file.txt'), b'hello', 1000000000)
        os.makedirs(os.path.join(self.root, 'a dir'))
        write(os.path.join(self.root, '.b file.txt.9-9.mft-part'), b'part')
        os.symlink('b file.txt', os.path.join(self.root, 'link'))
        os.symlink('missing', os.path.join(self.root, 'broken'))
        os.mkfifo(os.path.join(self.root, 'fifo'))
        entries = {e.name: e for e in self.client.list('.')}
        self.assertEqual(set(entries), {'b file.txt', 'a dir', 'link'})
        self.assertEqual(entries['b file.txt'], mft.Entry('f', 5, 1000000000, 'b file.txt'))
        self.assertEqual(entries['a dir'].kind, 'd')
        self.assertEqual(entries['link'].size, 5)
        self.assertEqual(self.client.stat('b file.txt'), mft.Entry('f', 5, 1000000000, 'b file.txt'))
        self.assertEqual(self.client.stat('a dir').kind, 'd')
        self.assertEqual(self.client.stat('.').kind, 'd')
        for path, code in (('missing', 'ENOENT'), ('../x', 'EINVAL')):
            with self.assertRaises(mft.MftError) as cm:
                self.client.stat(path)
            self.assertEqual(cm.exception.code, code)
        with self.assertRaises(mft.MftError) as cm:
            self.client.list('b file.txt')
        self.assertEqual(cm.exception.code, 'ENOTDIR')
        self.assertTrue(self.client.connected)

    def test_mkdir_rename_delete(self):
        self.client.mkdir('new folder')
        self.assertTrue(os.path.isdir(os.path.join(self.root, 'new folder')))
        with self.assertRaises(mft.MftError) as cm:
            self.client.mkdir('new folder')
        self.assertEqual(cm.exception.code, 'EEXIST')
        with self.assertRaises(mft.MftError) as cm:
            self.client.mkdir('no/such')
        self.assertEqual(cm.exception.code, 'ENOENT')
        write(os.path.join(self.root, 'new folder', 'x'), b'x')
        write(os.path.join(self.root, 'other'), b'o')
        with self.assertRaises(mft.MftError) as cm:
            self.client.rename('other', 'new folder')
        self.assertEqual(cm.exception.code, 'EEXIST')
        self.client.rename('new folder', 'renamed')
        self.assertTrue(os.path.exists(os.path.join(self.root, 'renamed', 'x')))
        with self.assertRaises(mft.MftError) as cm:
            self.client.rename('gone', 'z')
        self.assertEqual(cm.exception.code, 'ENOENT')
        self.client.delete('renamed')
        self.client.delete('other')
        self.assertEqual(os.listdir(self.root), [])
        with self.assertRaises(mft.MftError) as cm:
            self.client.delete('.')
        self.assertEqual(cm.exception.code, 'EINVAL')
        self.client.quit()
        self.assertFalse(self.client.connected)

    def test_abort_request_ignored(self):
        raw = Raw(self.server.port)
        raw.ch.send_line('A')
        self.assertEqual(raw.call('LIST .'), 'OK 0')
        self.assertTrue(raw.call('NOPE x').startswith('ERR EINVAL'))
        raw.close()


class Idle(ServerCase):
    def test_idle_session(self):
        """A session without requests lasts longer than IO_TIMEOUT."""
        saved = mft.IO_TIMEOUT
        mft.IO_TIMEOUT = 0.3
        try:
            self.reconnect()
            time.sleep(1.0)
            self.assertEqual(self.client.list('.'), [])
        finally:
            mft.IO_TIMEOUT = saved


class Copies(ServerCase):
    def test_get_and_put(self):
        big = os.urandom(3000000)
        files = {'small.txt': b'hello\n', 'empty': b'', 'big name with spaces.wav': big}
        for name, data in files.items():
            write(os.path.join(self.root, name), data, 1500000000)
        for name, data in files.items():
            e = self.client.stat(name)
            got = self.client.get(name, self.local, e.size, e.mtime)
            self.assertEqual(got, len(data))
            self.assertEqual(read(os.path.join(self.local, name)), data)
            self.assertEqual(int(os.stat(os.path.join(self.local, name)).st_mtime), 1500000000)
        self.assertEqual([n for n in os.listdir(self.local) if mft.is_part(n)], [])
        self.assertIn('sent big name with spaces.wav, 3000000 bytes', self.log)
        os.makedirs(os.path.join(self.root, 'up'))
        for name, data in files.items():
            sent = self.client.put(os.path.join(self.local, name), 'up/' + name)
            self.assertEqual(sent, len(data))
            self.assertEqual(read(os.path.join(self.root, 'up', name)), data)
            self.assertEqual(int(os.stat(os.path.join(self.root, 'up', name)).st_mtime), 1500000000)
        self.assertIn('stored up/big name with spaces.wav, 3000000 bytes', self.log)
        with self.assertRaises(mft.MftError) as cm:
            self.client.put(os.path.join(self.local, 'small.txt'), 'up')
        self.assertEqual(cm.exception.code, 'EISDIR')
        with self.assertRaises(mft.MftError) as cm:
            self.client.put(os.path.join(self.local, 'small.txt'), 'no/such/file')
        self.assertEqual(cm.exception.code, 'ENOENT')
        self.assertTrue(self.client.connected)

    def test_trees(self):
        src = os.path.join(self.local, 'tree')
        write(os.path.join(src, 'a.txt'), b'a')
        write(os.path.join(src, 'sub dir', 'b.bin'), os.urandom(70000))
        os.makedirs(os.path.join(src, 'empty'))
        self.client.put_tree(src, '.')
        for rel in ('a.txt', 'sub dir/b.bin'):
            self.assertEqual(read(os.path.join(self.root, 'tree', rel)), read(os.path.join(src, rel)))
        self.assertTrue(os.path.isdir(os.path.join(self.root, 'tree', 'empty')))
        # A second upload into the existing folders succeeds.
        self.client.put_tree(src, '.')
        out = os.path.join(self.tmp, 'out')
        os.makedirs(out)
        self.client.get_tree('tree', out)
        for rel in ('a.txt', 'sub dir/b.bin'):
            self.assertEqual(read(os.path.join(out, 'tree', rel)), read(os.path.join(src, rel)))
        self.assertTrue(os.path.isdir(os.path.join(out, 'tree', 'empty')))
        jobs = self.client.plan_get('tree', 'd', 0, 0, out)
        self.assertEqual([j.label for j in jobs], ['tree', 'tree/a.txt', 'tree/empty', 'tree/sub dir', 'tree/sub dir/b.bin'])

    def test_resume_get(self):
        data = os.urandom(500000)
        write(os.path.join(self.root, 'f.bin'), data, 1600000000)
        write(os.path.join(self.local, mft.part_name('f.bin', len(data), 1600000000)), data[:200000])
        write(os.path.join(self.local, mft.part_name('f.bin', 7, 1)), b'stale')
        calls = []
        got = self.client.get('f.bin', self.local, len(data), 1600000000, progress=lambda d, t: calls.append(d))
        self.assertEqual(calls[0], 200000)
        self.assertEqual(got, 300000)
        self.assertEqual(read(os.path.join(self.local, 'f.bin')), data)
        self.assertEqual(os.listdir(self.local), ['f.bin'])
        self.assertIn('sent f.bin, 500000 bytes, resumed at 200000', self.log)

    def test_resume_put(self):
        data = os.urandom(500000)
        write(os.path.join(self.local, 'f.bin'), data, 1600000000)
        write(os.path.join(self.root, mft.part_name('f.bin', len(data), 1600000000)), data[:123456])
        calls = []
        sent = self.client.put(os.path.join(self.local, 'f.bin'), 'f.bin', progress=lambda d, t: calls.append(d))
        self.assertEqual(calls[0], 123456)
        self.assertEqual(sent, 500000 - 123456)
        self.assertEqual(read(os.path.join(self.root, 'f.bin')), data)
        self.assertEqual(os.listdir(self.root), ['f.bin'])

    def test_cancel_get_then_resume(self):
        data = os.urandom(4000000)
        write(os.path.join(self.root, 'c.bin'), data, 1700000000)
        cancel = threading.Event()

        def progress(done, total):
            if done >= 1000000:
                cancel.set()

        with self.assertRaises(mft.MftError) as cm:
            self.client.get('c.bin', self.local, len(data), 1700000000, progress, cancel)
        self.assertEqual(cm.exception.code, 'ECANCELED')
        part = os.path.join(self.local, mft.part_name('c.bin', len(data), 1700000000))
        have = os.path.getsize(part)
        self.assertTrue(1000000 <= have < len(data))
        # The session is still usable after the cancel.
        self.assertTrue(self.client.connected)
        self.assertEqual(self.client.stat('c.bin').size, len(data))
        got = self.client.get('c.bin', self.local, len(data), 1700000000)
        self.assertEqual(got, len(data) - have)
        self.assertEqual(read(os.path.join(self.local, 'c.bin')), data)

    def test_cancel_put_then_resume(self):
        data = os.urandom(4000000)
        write(os.path.join(self.local, 'c.bin'), data)
        cancel = threading.Event()

        def progress(done, total):
            if done >= 1000000:
                cancel.set()

        with self.assertRaises(mft.MftError) as cm:
            self.client.put(os.path.join(self.local, 'c.bin'), 'c.bin', progress, cancel)
        self.assertEqual(cm.exception.code, 'ECANCELED')
        self.assertTrue(self.client.connected)
        self.assertFalse(os.path.exists(os.path.join(self.root, 'c.bin')))
        parts = [n for n in os.listdir(self.root) if mft.is_part(n)]
        self.assertEqual(len(parts), 1)
        have = os.path.getsize(os.path.join(self.root, parts[0]))
        self.assertTrue(have >= 1000000)
        sent = self.client.put(os.path.join(self.local, 'c.bin'), 'c.bin')
        self.assertEqual(sent, len(data) - have)
        self.assertEqual(read(os.path.join(self.root, 'c.bin')), data)

    def test_stale(self):
        write(os.path.join(self.root, 's.txt'), b'version 1', 1000)
        e = self.client.stat('s.txt')
        write(os.path.join(self.root, 's.txt'), b'version two', 2000)
        with self.assertRaises(mft.MftError) as cm:
            self.client.get('s.txt', self.local, e.size, e.mtime)
        self.assertEqual(cm.exception.code, 'ESTALE')
        self.assertTrue(self.client.connected)

    def test_bad_checksum(self):
        raw = Raw(self.server.port)
        good, bad = b'first chunk ', b'second chunk'
        self.assertEqual(raw.call(f'PUT x.txt {len(good) + len(bad)} 100'), 'OK 0')
        raw.ch.send_chunk(good)
        raw.ch.send(b'C %d %08x\n' % (len(bad), zlib.crc32(bad) ^ 1) + bad)
        raw.ch.send_chunk(b'after')
        self.assertEqual(raw.call('E'), 'ERR EIO checksum mismatch')
        self.assertFalse(os.path.exists(os.path.join(self.root, 'x.txt')))
        part = os.path.join(self.root, mft.part_name('x.txt', len(good) + len(bad), 100))
        self.assertEqual(read(part), good)
        # The session continues after the error reply.
        self.assertEqual(raw.call('LIST .'), 'OK 0')
        raw.close()

    def test_short_data(self):
        raw = Raw(self.server.port)
        self.assertEqual(raw.call('PUT y.txt 10 100'), 'OK 0')
        raw.ch.send_chunk(b'12345')
        self.assertTrue(raw.call('E').startswith('ERR EIO'))
        raw.close()


try:
    import tkinter
    _root = tkinter.Tk()
    _root.withdraw()
    _root.destroy()
    HAVE_TK = True
except Exception:  # noqa: BLE001 - no display or no Tk
    HAVE_TK = False


@unittest.skipUnless(HAVE_TK, 'Tk cannot open a window')
class Window(ServerCase):
    def setUp(self):
        super().setUp()
        import tkinter
        self.tk_root = tkinter.Tk()
        self.tk_root.withdraw()
        self.cwd = os.getcwd()
        os.chdir(self.local)
        self.app = transfer.TransferApp(self.tk_root, '127.0.0.1', self.server.port)
        self.answers = []
        self.app.ask_yes_no = lambda title, message: self.answers.append(message) or True

    def tearDown(self):
        self.app.close()
        os.chdir(self.cwd)
        super().tearDown()

    def wait(self, cond, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            self.tk_root.update()
            self.app.process_events()
            if cond():
                return
            time.sleep(0.01)
        self.fail('timeout')

    def select(self, pane, name):
        item = pane.item_of(name)
        self.assertIsNotNone(item, name)
        pane.tree.selection_set(item)

    def test_window(self):
        write(os.path.join(self.local, 'up.txt'), b'upload me')
        write(os.path.join(self.root, 'down.txt'), b'download me')
        os.makedirs(os.path.join(self.root, 'remote dir'))
        self.app.local.refresh()
        self.assertEqual(self.app.local.names(), {'up.txt'})
        self.app.connect()
        self.wait(lambda: self.app.connected and self.app.remote.names() == {'down.txt', 'remote dir'})
        self.assertIn('served', self.app.remote.title.cget('text'))
        # Upload and download through the buttons.
        self.select(self.app.local, 'up.txt')
        self.app.upload()
        self.wait(lambda: not self.app.busy and os.path.exists(os.path.join(self.root, 'up.txt')))
        self.assertEqual(read(os.path.join(self.root, 'up.txt')), b'upload me')
        self.select(self.app.remote, 'down.txt')
        self.app.download()
        self.wait(lambda: not self.app.busy and 'down.txt' in self.app.local.names())
        self.assertEqual(read(os.path.join(self.local, 'down.txt')), b'download me')
        self.assertIn('1 of 1 files', self.app.summary.cget('text'))
        # A second download asks before it replaces the file.
        self.select(self.app.remote, 'down.txt')
        self.app.download()
        self.wait(lambda: self.answers and not self.app.busy)
        self.assertIn('down.txt', self.answers[0])
        # A drag of a local file onto the remote folder row uploads into the folder.
        self.wait(lambda: 'up.txt' in self.app.remote.names())
        self.app.drag_start(self.app.local, [e for e in self.app.local.rows.values() if e.name == 'up.txt'])
        self.app.drag_drop(self.app.remote, self.app.remote.item_of('remote dir'))
        self.wait(lambda: not self.app.busy and os.path.exists(os.path.join(self.root, 'remote dir', 'up.txt')))
        # A drag of a remote file onto the empty part of the local pane downloads it.
        os.remove(os.path.join(self.local, 'down.txt'))
        self.app.drag_start(self.app.remote, [e for e in self.app.remote.rows.values() if e.name == 'down.txt'])
        self.app.drag_drop(self.app.local, None)
        self.wait(lambda: not self.app.busy and os.path.exists(os.path.join(self.local, 'down.txt')))
        # Remote file management.
        self.app.remote.new_folder('made here')
        self.wait(lambda: 'made here' in self.app.remote.names())
        self.select(self.app.remote, 'made here')
        self.app.remote.rename('renamed')
        self.wait(lambda: 'renamed' in self.app.remote.names())
        self.select(self.app.remote, 'renamed')
        self.app.remote.delete(confirm=False)
        self.wait(lambda: 'renamed' not in self.app.remote.names())
        self.assertFalse(os.path.exists(os.path.join(self.root, 'renamed')))
        # The window serves a folder of its own.
        self.app.start_server(self.local, 0)
        self.assertIsNotNone(self.app.server)
        probe = mft.Client().connect('127.0.0.1', self.app.server.port)
        self.wait(lambda: (self.app.update_server_status() or True) and '1 client' in self.app.serve_status.cget('text'))
        self.assertIn('down.txt', {e.name for e in probe.list('.')})
        probe.quit()
        self.app.stop_server()
        self.app.disconnect()
        self.wait(lambda: not self.app.connected)


if __name__ == '__main__':
    unittest.main()
