#!/usr/bin/env python3
"""Transfer: copies files between the host and minios over MFT 1.

    transfer.py [gui] [HOST[:PORT]]
    transfer.py serve [-p PORT] [DIR]
    transfer.py ls [-h HOST] [-p PORT] [PATH]
    transfer.py get [-h HOST] [-p PORT] [-o DIR] PATH...
    transfer.py put [-h HOST] [-p PORT] [-d PATH] FILE...
    transfer.py mkdir|rm [-h HOST] [-p PORT] PATH...
    transfer.py mv [-h HOST] [-p PORT] FROM TO

HOST defaults to 127.0.0.1 and PORT to 9102, the minios server through
the port forward of tools/run.sh. serve uses the port 9103 and the
current folder by default. The protocol is in docs/design/filetransfer.md.
"""
import getopt
import os
import queue
import shutil
import sys
import threading
import time

# No __pycache__ folders in the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mft  # noqa: E402

DEFAULT_HOST = '127.0.0.1'


def human_size(n):
    """The size with the unit B, KiB, MiB or GiB."""
    if n < 1024:
        return f'{n} B'
    for unit in ('KiB', 'MiB', 'GiB'):
        n /= 1024
        if n < 1024 or unit == 'GiB':
            return f'{n:.1f} {unit}'


def format_time(mtime):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(mtime))


def sort_entries(entries):
    """Folders first, then files, each group in the order of the names."""
    return sorted(entries, key=lambda e: (e.kind != 'd', e.name.casefold(), e.name))


# ---- command line ----

class TerminalProgress:
    """Once per second, when standard error is a terminal, one line with
    the bytes of the current file, overwritten in place."""

    def __init__(self, label):
        self.label = label
        self.tty = sys.stderr.isatty()
        self.shown = time.time()
        self.drawn = False

    def __call__(self, done, total):
        now = time.time()
        if self.tty and now - self.shown >= 1:
            self.shown = now
            self.drawn = True
            sys.stderr.write(f'\r{self.label}: {done} of {total} bytes')
            sys.stderr.flush()

    def end(self):
        if self.drawn:
            sys.stderr.write('\n')
            sys.stderr.flush()


def usage():
    sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
    sys.exit(2)


def parse_args(args, letters):
    try:
        opts, rest = getopt.getopt(args, letters)
    except getopt.GetoptError as e:
        sys.stderr.write(f'transfer: {e}\n')
        usage()
    return dict(opts), rest


def connect(opts):
    client = mft.Client()
    host = opts.get('-h', DEFAULT_HOST)
    port = opts.get('-p', str(mft.GUEST_PORT))
    try:
        client.connect(host, int(port))
    except (mft.MftError, ValueError) as e:
        sys.stderr.write(f'transfer: {host}:{port}: {e}\n')
        sys.exit(1)
    return client


def run_jobs(client, jobs):
    """Runs the jobs and prints one line per copied file. Returns the
    number of failures."""
    failures = 0
    for job in jobs:
        progress = TerminalProgress(job.label)
        try:
            client.run(job, progress)
        except mft.MftError as e:
            progress.end()
            sys.stderr.write(f'transfer: {job.label}: {e}\n')
            failures += 1
            if not client.connected:
                break
            continue
        progress.end()
        if job.kind == 'f':
            verb = 'downloaded' if job.direction == 'get' else 'uploaded'
            print(f'{verb} {job.remote}, {job.size} bytes', flush=True)
    return failures


def cmd_serve(args):
    opts, rest = parse_args(args, 'p:')
    if len(rest) > 1:
        usage()
    root = rest[0] if rest else '.'

    def log(line):
        print(line, flush=True)

    server = mft.Server(root, int(opts.get('-p', mft.HOST_PORT)), log=log)
    try:
        server.start()
    except mft.MftError as e:
        sys.stderr.write(f'transfer: serve {root}: {e}\n')
        return 1
    print(f'serving {server.root} on port {server.port}', flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
    return 0


def cmd_ls(args):
    opts, rest = parse_args(args, 'h:p:')
    if len(rest) > 1:
        usage()
    client = connect(opts)
    try:
        for e in sort_entries(client.list(rest[0] if rest else '.')):
            name = e.name + ('/' if e.kind == 'd' else '')
            print(f'{e.kind} {e.size:>12} {format_time(e.mtime)} {name}')
    except mft.MftError as e:
        sys.stderr.write(f'transfer: {rest[0] if rest else "."}: {e}\n')
        return 1
    finally:
        client.close()
    return 0


def cmd_get(args):
    opts, rest = parse_args(args, 'h:p:o:')
    if not rest:
        usage()
    out = opts.get('-o', '.')
    client = connect(opts)
    failures = 0
    try:
        for path in rest:
            try:
                e = client.stat(path)
                jobs = client.plan_get(path, e.kind, e.size, e.mtime, out)
            except mft.MftError as e:
                sys.stderr.write(f'transfer: {path}: {e}\n')
                failures += 1
                continue
            failures += run_jobs(client, jobs)
    finally:
        client.close()
    return 1 if failures else 0


def cmd_put(args):
    opts, rest = parse_args(args, 'h:p:d:')
    if not rest:
        usage()
    target = opts.get('-d', '.')
    client = connect(opts)
    failures = 0
    try:
        for path in rest:
            try:
                jobs = client.plan_put(path, target)
            except mft.MftError as e:
                sys.stderr.write(f'transfer: {path}: {e}\n')
                failures += 1
                continue
            failures += run_jobs(client, jobs)
    finally:
        client.close()
    return 1 if failures else 0


def cmd_simple(args, request):
    opts, rest = parse_args(args, 'h:p:')
    if not rest or (request == 'mv' and len(rest) != 2):
        usage()
    client = connect(opts)
    failures = 0
    try:
        if request == 'mv':
            try:
                client.rename(rest[0], rest[1])
            except mft.MftError as e:
                sys.stderr.write(f'transfer: {rest[0]}: {e}\n')
                failures += 1
        else:
            for path in rest:
                try:
                    client.mkdir(path) if request == 'mkdir' else client.delete(path)
                except mft.MftError as e:
                    sys.stderr.write(f'transfer: {path}: {e}\n')
                    failures += 1
    finally:
        client.close()
    return 1 if failures else 0


# ---- window ----

class Worker(threading.Thread):
    """Runs the client requests of the window in order, in one thread.

    submit queues a function. The worker calls the function with the
    client. The result or the exception goes to the event queue of the
    window, which calls done or fail on the Tk thread."""

    def __init__(self, events):
        super().__init__(name='transfer-client', daemon=True)
        self.commands = queue.Queue()
        self.events = events
        self.client = mft.Client()
        self.start()

    def run(self):
        while True:
            item = self.commands.get()
            if item is None:
                self.client.close()
                return
            fn, done, fail = item
            try:
                result = fn(self.client)
            except Exception as e:  # noqa: BLE001 - every failure goes to the window
                self.events.put((fail, (e,)))
            else:
                self.events.put((done, (result,)))

    def submit(self, fn, done=None, fail=None):
        self.commands.put((fn, done, fail))

    def stop(self):
        self.commands.put(None)


class Pane:
    """One file pane: Up, the path, the table and the buttons."""

    def __init__(self, app, parent, title, remote):
        import tkinter as tk
        from tkinter import ttk
        self.app, self.remote = app, remote
        self.cwd = '.' if remote else os.path.abspath(os.getcwd())
        self.rows = {}                      # Treeview item -> mft.Entry
        self.frame = ttk.Frame(parent, padding=4)
        self.title = ttk.Label(self.frame, text=title, font=('TkDefaultFont', 0, 'bold'))
        self.title.pack(anchor='w')
        bar = ttk.Frame(self.frame)
        bar.pack(fill='x', pady=(4, 2))
        self.up = ttk.Button(bar, text='Up', width=4, command=self.go_up)
        self.up.pack(side='left')
        self.path = ttk.Label(bar, text='', anchor='w')
        self.path.pack(side='left', fill='x', expand=True, padx=6)
        body = ttk.Frame(self.frame)
        body.pack(fill='both', expand=True)
        self.tree = ttk.Treeview(body, columns=('name', 'size', 'modified'), show='headings', selectmode='extended')
        for col, text, width, anchor in (('name', 'Name', 220, 'w'), ('size', 'Size', 80, 'e'),
                                         ('modified', 'Modified', 130, 'w')):
            self.tree.heading(col, text=text, anchor=anchor)
            self.tree.column(col, width=width, anchor=anchor, stretch=(col == 'name'))
        self.tree.tag_configure('drop', background='#cfe3ff')
        scroll = ttk.Scrollbar(body, orient='vertical', command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        buttons = ttk.Frame(self.frame)
        buttons.pack(fill='x', pady=(4, 0))
        self.buttons = []
        for text, cmd in (('New folder', self.new_folder), ('Rename', self.rename),
                          ('Delete', self.delete), ('Refresh', self.refresh)):
            b = ttk.Button(buttons, text=text, command=cmd)
            b.pack(side='left', padx=(0, 4))
            self.buttons.append(b)
        if remote:
            self.copy = ttk.Button(buttons, text='Download', command=app.download)
        else:
            self.copy = ttk.Button(buttons, text='Upload', command=app.upload)
        self.copy.pack(side='right')
        self.buttons.append(self.copy)
        self.tree.bind('<Double-1>', self.on_double)
        self.tree.bind('<ButtonPress-1>', lambda e: app.on_press(self, e), add='+')
        self.tree.bind('<B1-Motion>', lambda e: app.on_motion(self, e), add='+')
        self.tree.bind('<ButtonRelease-1>', lambda e: app.on_release(self, e), add='+')
        self.tk = tk

    # ---- contents ----

    def show(self, entries):
        self.tree.delete(*self.tree.get_children())
        self.rows = {}
        for e in sort_entries(entries):
            item = self.tree.insert('', 'end', values=(e.name + ('/' if e.kind == 'd' else ''),
                                                       '' if e.kind == 'd' else human_size(e.size),
                                                       format_time(e.mtime)))
            self.rows[item] = e
        self.path.configure(text=self.display_path())
        self.up.state(['!disabled'] if self.can_go_up() else ['disabled'])

    def display_path(self):
        if not self.remote:
            return self.cwd
        return '/' if self.cwd == '.' else '/' + self.cwd

    def can_go_up(self):
        if self.remote:
            return self.app.connected and self.cwd != '.'
        return os.path.dirname(self.cwd) != self.cwd

    def join(self, name):
        return mft.join(self.cwd, name) if self.remote else os.path.join(self.cwd, name)

    def selected(self):
        return [self.rows[i] for i in self.tree.selection() if i in self.rows]

    def item_of(self, name):
        for item, e in self.rows.items():
            if e.name == name:
                return item
        return None

    def names(self):
        return {e.name for e in self.rows.values()}

    def refresh(self):
        if self.remote:
            if not self.app.connected:
                self.show([])
                return
            cwd = self.cwd
            self.app.worker.submit(lambda c: c.list(cwd), self.show, self.app.report)
            return
        try:
            self.show(mft.local_entries(self.cwd))
        except mft.MftError as e:
            self.app.report(e)

    def open(self, path):
        self.cwd = path
        self.refresh()

    def go_up(self):
        if self.can_go_up():
            self.open(mft.parent_of(self.cwd) if self.remote else os.path.dirname(self.cwd))

    def on_double(self, event):
        item = self.tree.identify_row(event.y)
        e = self.rows.get(item)
        if e and e.kind == 'd':
            self.open(self.join(e.name))

    # ---- file management ----

    def new_folder(self, name=None):
        name = name or self.app.ask_string('New folder', 'Name of the new folder:', '')
        if not name:
            return
        path = self.join(name)
        if self.remote:
            self.app.worker.submit(lambda c: c.mkdir(path), lambda r: self.refresh(), self.app.report)
            return
        try:
            os.mkdir(path)
        except OSError as e:
            self.app.report(mft.error_from_os(e))
        self.refresh()

    def rename(self, new=None):
        sel = self.selected()
        if len(sel) != 1:
            self.app.set_status('Select one item to rename.')
            return
        old = sel[0].name
        new = new or self.app.ask_string('Rename', f'New name of {old}:', old)
        if not new or new == old:
            return
        src, dst = self.join(old), self.join(new)
        if self.remote:
            self.app.worker.submit(lambda c: c.rename(src, dst), lambda r: self.refresh(), self.app.report)
            return
        try:
            if os.path.lexists(dst):
                raise mft.MftError('EEXIST', 'File exists')
            os.rename(src, dst)
        except OSError as e:
            self.app.report(mft.error_from_os(e))
        except mft.MftError as e:
            self.app.report(e)
        self.refresh()

    def delete(self, confirm=True):
        sel = self.selected()
        if not sel:
            return
        what = sel[0].name if len(sel) == 1 else f'{len(sel)} items'
        if confirm and not self.app.ask_yes_no('Delete', f'Delete {what}? Folders are removed with their contents.'):
            return
        paths = [self.join(e.name) for e in sel]
        if self.remote:
            def remove(c):
                for p in paths:
                    c.delete(p)
            self.app.worker.submit(remove, lambda r: self.refresh(), lambda e: (self.app.report(e), self.refresh()))
            return
        try:
            for p in paths:
                if os.path.isdir(p) and not os.path.islink(p):
                    shutil.rmtree(p)
                else:
                    os.remove(p)
        except OSError as e:
            self.app.report(mft.error_from_os(e))
        self.refresh()

    def set_enabled(self, enabled):
        for b in self.buttons:
            b.state(['!disabled'] if enabled else ['disabled'])


class TransferApp:
    """The window of docs/design/filetransfer.md."""

    POLL_MS = 50

    def __init__(self, root, host=DEFAULT_HOST, port=mft.GUEST_PORT):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.root = tk, root
        root.title('Transfer')
        root.geometry('980x680')
        root.minsize(720, 480)
        self.events = queue.Queue()
        self.worker = Worker(self.events)
        self.connected = False
        self.server = None
        self.cancel = threading.Event()
        self.busy = False
        self.drag = None                    # (pane, entries) of a running drag
        self.press = None                   # (pane, x, y) of a press that may start a drag
        self.drop_item = None
        self.jobs = []                      # (job, Treeview item) of the running batch
        self.stats = None

        outer = ttk.Frame(root, padding=8)
        outer.pack(fill='both', expand=True)

        conn = ttk.Frame(outer)
        conn.pack(fill='x')
        ttk.Label(conn, text='Host').pack(side='left')
        self.host = tk.StringVar(value=host)
        ttk.Entry(conn, textvariable=self.host, width=22).pack(side='left', padx=(4, 10))
        ttk.Label(conn, text='Port').pack(side='left')
        self.port = tk.StringVar(value=str(port))
        ttk.Entry(conn, textvariable=self.port, width=7).pack(side='left', padx=(4, 10))
        self.connect_button = ttk.Button(conn, text='Connect', command=self.toggle_connection)
        self.connect_button.pack(side='left')

        serve = ttk.Frame(outer)
        serve.pack(fill='x', pady=(6, 0))
        self.serving = tk.BooleanVar(value=False)
        ttk.Checkbutton(serve, text='Serve', variable=self.serving, command=self.toggle_server).pack(side='left')
        self.serve_dir = tk.StringVar(value=os.path.abspath(os.getcwd()))
        ttk.Entry(serve, textvariable=self.serve_dir, width=36).pack(side='left', padx=(4, 4))
        ttk.Button(serve, text='Choose', command=self.choose_folder).pack(side='left', padx=(0, 10))
        ttk.Label(serve, text='Port').pack(side='left')
        self.serve_port = tk.StringVar(value=str(mft.HOST_PORT))
        ttk.Entry(serve, textvariable=self.serve_port, width=7).pack(side='left', padx=(4, 10))
        self.serve_status = ttk.Label(serve, text='Not serving')
        self.serve_status.pack(side='left')

        panes = ttk.PanedWindow(outer, orient='horizontal')
        panes.pack(fill='both', expand=True, pady=(8, 0))
        self.local = Pane(self, panes, 'This computer', remote=False)
        self.remote = Pane(self, panes, 'Not connected', remote=True)
        panes.add(self.local.frame, weight=1)
        panes.add(self.remote.frame, weight=1)
        self.remote.set_enabled(False)

        transfers = ttk.Frame(outer)
        transfers.pack(fill='x', pady=(8, 0))
        self.list = ttk.Treeview(transfers, columns=('name', 'direction', 'size', 'status'), show='headings', height=6)
        for col, text, width, anchor in (('name', 'Name', 360, 'w'), ('direction', 'Direction', 90, 'w'),
                                         ('size', 'Size', 90, 'e'), ('status', 'Status', 220, 'w')):
            self.list.heading(col, text=text, anchor=anchor)
            self.list.column(col, width=width, anchor=anchor, stretch=(col in ('name', 'status')))
        self.list.pack(fill='x')
        bar = ttk.Frame(transfers)
        bar.pack(fill='x', pady=(4, 0))
        self.progress = ttk.Progressbar(bar, orient='horizontal', mode='determinate', maximum=1000, length=240)
        self.progress.pack(side='left')
        self.summary = ttk.Label(bar, text='No transfers')
        self.summary.pack(side='left', padx=8)
        self.cancel_button = ttk.Button(bar, text='Cancel', command=self.cancel_transfers)
        self.cancel_button.pack(side='right')
        self.cancel_button.state(['disabled'])

        self.status = ttk.Label(outer, text='Ready', anchor='w')
        self.status.pack(fill='x', pady=(6, 0))

        root.protocol('WM_DELETE_WINDOW', self.close)
        self.local.refresh()
        self.remote.show([])
        self.root.after(self.POLL_MS, self.poll)

    # ---- dialogs, replaceable by the tests ----

    def ask_string(self, title, prompt, initial):
        from tkinter import simpledialog
        return simpledialog.askstring(title, prompt, initialvalue=initial, parent=self.root)

    def ask_yes_no(self, title, message):
        from tkinter import messagebox
        return messagebox.askyesno(title, message, parent=self.root)

    def choose_folder(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(initialdir=self.serve_dir.get(), parent=self.root)
        if folder:
            self.serve_dir.set(folder)

    # ---- events ----

    def poll(self):
        self.process_events()
        self.update_server_status()
        self.root.after(self.POLL_MS, self.poll)

    def process_events(self):
        while True:
            try:
                fn, args = self.events.get_nowait()
            except queue.Empty:
                return
            if fn:
                fn(*args)

    def post(self, fn, *args):
        """Queues a call for the Tk thread. Other threads use only this."""
        self.events.put((fn, args))

    def set_status(self, text):
        self.status.configure(text=text)

    def report(self, error):
        self.set_status(f'Error: {error}')

    # ---- connection ----

    def toggle_connection(self):
        if self.connected:
            self.disconnect()
        else:
            self.connect()

    def connect(self, host=None, port=None):
        host = host or self.host.get().strip()
        try:
            port = int(port or self.port.get())
        except ValueError:
            self.set_status('The port is not a number.')
            return
        self.connect_button.state(['disabled'])
        self.set_status(f'Connecting to {host}:{port}')

        def done(client):
            self.connected = True
            self.connect_button.configure(text='Disconnect')
            self.connect_button.state(['!disabled'])
            self.remote.title.configure(text=f'{host}:{port}  {client.server_name}')
            self.remote.set_enabled(True)
            self.remote.open('.')
            self.set_status(f'Connected to {host}:{port}')

        def fail(e):
            self.connect_button.state(['!disabled'])
            self.report(e)

        self.worker.submit(lambda c: c.connect(host, port), done, fail)

    def disconnect(self):
        self.cancel.set()

        def done(_):
            self.connected = False
            self.connect_button.configure(text='Connect')
            self.remote.title.configure(text='Not connected')
            self.remote.cwd = '.'
            self.remote.show([])
            self.remote.set_enabled(False)
            self.set_status('Disconnected')

        def quit_client(c):
            try:
                c.quit()
            except mft.MftError:
                c.close()

        self.worker.submit(quit_client, done, done)

    # ---- server ----

    def toggle_server(self):
        if self.serving.get():
            self.start_server()
        else:
            self.stop_server()

    def start_server(self, folder=None, port=None):
        folder = folder or self.serve_dir.get()
        try:
            port = int(self.serve_port.get() if port is None else port)
            self.server = mft.Server(folder, port, log=lambda line: self.post(self.set_status, line)).start()
        except (mft.MftError, ValueError) as e:
            self.server = None
            self.serving.set(False)
            self.report(e)
            return
        self.serving.set(True)
        self.serve_port.set(str(self.server.port))
        self.set_status(f'Serving {self.server.root} on port {self.server.port}')

    def stop_server(self):
        if self.server:
            self.server.stop()
            self.server = None
        self.serving.set(False)
        self.set_status('The server is stopped.')

    def update_server_status(self):
        if self.server:
            n = self.server.clients
            self.serve_status.configure(text=f'Port {self.server.port}, {n} client{"" if n == 1 else "s"} connected')
        else:
            self.serve_status.configure(text='Not serving')

    # ---- transfers ----

    def upload(self, entries=None, target=None):
        """Copies local entries into the remote folder target."""
        if not self.connected:
            self.set_status('Connect to a server first.')
            return
        entries = self.local.selected() if entries is None else entries
        if not entries:
            return
        target = self.remote.cwd if target is None else target
        paths = [os.path.join(self.local.cwd, e.name) for e in entries]
        names = [e.name for e in entries]

        def plan(c):
            existing = {e.name for e in c.list(target)}
            jobs = []
            for p in paths:
                jobs += c.plan_put(p, target)
            return jobs, sorted(existing & set(names))

        self.worker.submit(plan, self.confirm_and_run, self.report)

    def download(self, entries=None, target=None):
        """Copies remote entries into the local folder target."""
        if not self.connected:
            self.set_status('Connect to a server first.')
            return
        entries = self.remote.selected() if entries is None else entries
        if not entries:
            return
        target = self.local.cwd if target is None else target
        cwd = self.remote.cwd
        names = [e.name for e in entries]

        def plan(c):
            jobs = []
            for e in entries:
                jobs += c.plan_get(mft.join(cwd, e.name), e.kind, e.size, e.mtime, target)
            existing = {n for n in names if os.path.lexists(os.path.join(target, n))}
            return jobs, sorted(existing)

        self.worker.submit(plan, self.confirm_and_run, self.report)

    def confirm_and_run(self, result):
        jobs, conflicts = result
        if conflicts:
            what = conflicts[0] if len(conflicts) == 1 else f'{len(conflicts)} items'
            if not self.ask_yes_no('Replace', f'The target folder already contains {what}. Replace?'):
                self.set_status('The copy was not started.')
                return
        self.start_jobs(jobs)

    def start_jobs(self, jobs):
        if self.busy:
            self.set_status('A transfer is running.')
            return
        self.busy = True
        self.cancel.clear()
        self.cancel_button.state(['!disabled'])
        self.list.delete(*self.list.get_children())
        self.jobs = []
        for job in jobs:
            item = self.list.insert('', 'end', values=(
                job.label, 'Download' if job.direction == 'get' else 'Upload',
                '' if job.kind == 'd' else human_size(job.size), 'Waiting'))
            self.jobs.append((job, item))
        files = [j for j in jobs if j.kind == 'f']
        self.stats = {'files': len(files), 'files_done': 0, 'bytes': sum(j.size for j in files),
                      'bytes_done': 0, 'start': time.time(), 'moved': 0}
        self.update_summary()
        batch = list(self.jobs)
        cancel = self.cancel

        def run(c):
            for index, (job, item) in enumerate(batch):
                if cancel.is_set():
                    for _, rest in batch[index:]:
                        self.post(self.set_row, rest, 'Cancelled')
                    break
                self.post(self.set_row, item, 'Copying')
                last = [0.0, 0]

                def progress(done, total, item=item, job=job, last=last):
                    now = time.time()
                    if now - last[0] >= 0.1 or done == total:
                        last[0] = now
                        self.post(self.on_progress, item, job, done, total)

                try:
                    moved = c.run(job, progress, cancel)
                except mft.MftError as e:
                    self.post(self.set_row, item, 'Cancelled' if e.code == 'ECANCELED' else f'Failed: {e.message}')
                    if not c.connected:
                        for _, rest in batch[index + 1:]:
                            self.post(self.set_row, rest, 'Not started')
                        break
                    continue
                self.post(self.on_job_done, item, job, moved)
            return None

        self.worker.submit(run, self.on_batch_done, self.on_batch_failed)

    def set_row(self, item, status):
        if self.list.exists(item):
            self.list.set(item, 'status', status)

    def on_progress(self, item, job, done, total):
        self.set_row(item, f'{done * 100 // total if total else 100}%')
        self.progress['value'] = 1000 * done // total if total else 1000
        self.stats['current'] = done
        self.update_summary()

    def on_job_done(self, item, job, moved):
        resumed = job.kind == 'f' and moved < job.size
        self.set_row(item, f'Done, resumed at {human_size(job.size - moved)}' if resumed else 'Done')
        if job.kind == 'f':
            self.stats['files_done'] += 1
            self.stats['bytes_done'] += job.size
            self.stats['moved'] += moved
        self.stats['current'] = 0
        self.progress['value'] = 1000
        self.update_summary()

    def update_summary(self):
        s = self.stats
        if not s:
            self.summary.configure(text='No transfers')
            return
        done = s['bytes_done'] + s.get('current', 0)
        elapsed = max(time.time() - s['start'], 1e-3)
        rate = (s['moved'] + s.get('current', 0)) / elapsed
        self.summary.configure(text=f'{s["files_done"]} of {s["files"]} files, {human_size(done)} of '
                                    f'{human_size(s["bytes"])}, {human_size(int(rate))}/s')

    def finish_batch(self):
        self.busy = False
        self.cancel_button.state(['disabled'])
        self.local.refresh()
        self.remote.refresh()

    def on_batch_done(self, _):
        self.finish_batch()
        s = self.stats
        self.set_status('Cancelled' if self.cancel.is_set() else f'Copied {s["files_done"]} of {s["files"]} files')

    def on_batch_failed(self, e):
        self.finish_batch()
        self.report(e)

    def cancel_transfers(self):
        self.cancel.set()
        self.set_status('Cancelling')

    # ---- drag and drop between the panes ----

    def on_press(self, pane, event):
        self.press = (pane, event.x, event.y) if pane.tree.identify_row(event.y) else None

    def on_motion(self, pane, event):
        if self.drag is None:
            if not self.press or self.press[0] is not pane:
                return
            if abs(event.x - self.press[1]) <= 4 and abs(event.y - self.press[2]) <= 4:
                return
            self.drag_start(pane, pane.selected())
            return
        target = self.pane_at(event.x_root, event.y_root)
        item = None
        if target is not None and target is not self.drag[0]:
            y = event.y_root - target.tree.winfo_rooty()
            row = target.tree.identify_row(y)
            if row and target.rows.get(row) and target.rows[row].kind == 'd':
                item = row
        self.mark_drop(target, item)

    def on_release(self, pane, event):
        self.press = None
        if self.drag is None:
            return
        target = self.pane_at(event.x_root, event.y_root)
        item = None
        if target is not None:
            item = target.tree.identify_row(event.y_root - target.tree.winfo_rooty()) or None
        self.drag_drop(target, item)

    def pane_at(self, x, y):
        widget = self.root.winfo_containing(x, y)
        for pane in (self.local, self.remote):
            w = widget
            while w is not None:
                if w is pane.frame:
                    return pane
                w = w.master
        return None

    def drag_start(self, pane, entries):
        """Starts a drag of entries out of pane."""
        if not entries or (pane.remote and not self.connected):
            return
        self.drag = (pane, list(entries))
        self.root.configure(cursor='hand2')
        self.set_status(f'Dragging {len(entries)} item{"" if len(entries) == 1 else "s"}')

    def mark_drop(self, target, item):
        if self.drop_item and self.drop_item[1] != item:
            pane, old = self.drop_item
            if pane.tree.exists(old):
                pane.tree.item(old, tags=())
            self.drop_item = None
        if target is not None and item:
            target.tree.item(item, tags=('drop',))
            self.drop_item = (target, item)

    def drag_drop(self, target, item=None):
        """Ends the drag on the pane target, over the row item or not.

        A drop on a folder row copies into that folder. A drop elsewhere in
        the other pane copies into the folder of that pane."""
        drag, self.drag = self.drag, None
        self.root.configure(cursor='')
        self.mark_drop(None, None)
        if drag is None:
            return
        source, entries = drag
        if target is None or target is source:
            self.set_status('Ready')
            return
        folder = target.cwd
        row = target.rows.get(item) if item else None
        if row and row.kind == 'd':
            folder = target.join(row.name)
        if target.remote:
            self.upload(entries, folder)
        else:
            self.download(entries, folder)

    def close(self):
        self.cancel.set()
        if self.server:
            self.server.stop()
            self.server = None
        self.worker.stop()
        self.root.destroy()


def cmd_gui(args):
    if len(args) > 1:
        usage()
    host, port = DEFAULT_HOST, mft.GUEST_PORT
    if args:
        host, _, p = args[0].partition(':')
        if p:
            try:
                port = int(p)
            except ValueError:
                usage()
    import tkinter as tk
    root = tk.Tk()
    app = TransferApp(root, host, port)
    if args:
        app.connect(host, port)
    root.mainloop()
    return 0


COMMANDS = {
    'serve': cmd_serve, 'ls': cmd_ls, 'get': cmd_get, 'put': cmd_put,
    'mkdir': lambda a: cmd_simple(a, 'mkdir'), 'rm': lambda a: cmd_simple(a, 'rm'),
    'mv': lambda a: cmd_simple(a, 'mv'), 'gui': cmd_gui,
}


def main(argv):
    if argv and argv[0] in ('--help', 'help'):
        usage()
    if argv and argv[0] in COMMANDS:
        return COMMANDS[argv[0]](argv[1:])
    return cmd_gui(argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
