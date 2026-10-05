# File transfer: the MFT protocol and the Transfer programs

Transfer copies files between minios and the host. The host program is
`tools/transfer.py`, written in Python with Tkinter. The minios program is
`transfer`, written in Lua on the `gui`, `net`, `fs` and `thread` modules,
and installed by the package `transfer`. Both programs speak the protocol
MFT 1 of this document. Both programs can serve a folder and connect to a
server, at the same time. Both programs show the same window.

`xfer` (`user/coreutils/xfer.c` and `tools/xfer.py`) remains for scripts.
It speaks its own older protocol on the ports 9100 and 9101.

## Ports

| Port | Use |
| --- | --- |
| 9102 | The minios server. `tools/run.sh` forwards host port 9102 to guest port 9102. |
| 9103 | The host server. The guest reaches it at 10.0.2.2:9103 through the QEMU user network. |

Each program serves on its own port by default and connects to the port
of the other side by default. The host program connects to 127.0.0.1:9102.
The minios program connects to 10.0.2.2:9103.

## Protocol

MFT 1 runs on one TCP connection per session. The client sends requests.
The server answers each request in order. Requests and replies are lines
of UTF-8 text that end in LF. Fields are separated by one space. File data
travels in chunks between the lines.

### Paths

A path is relative to the folder of the server. Its components are
separated by `/`. The root folder is the path `.`. A path on the wire is
percent encoded: every byte outside `A-Z a-z 0-9 - . _ ~ /` is written as
`%` and two hexadecimal digits. Names with spaces therefore fit into one
field. The server decodes a path and rejects it with `EINVAL` when it is
empty, starts with `/`, or contains an empty component, a component `.`
or `..` after the root, or a NUL byte. A decoded path has at most 1024
bytes.

### Session start

The client sends `MFT 1`. The server answers `MFT 1 NAME`. NAME is the
percent encoded name of the served folder, for display. A server that
does not support version 1 answers `ERR EPROTO unsupported version` and
closes the connection.

### Replies and errors

A successful reply starts with `OK`. A failure is the line
`ERR CODE MESSAGE`. CODE is the POSIX name of the error, for example
`ENOENT`. MESSAGE is text for the user and may contain spaces. The codes
are `ENOENT`, `EEXIST`, `EACCES`, `EINVAL`, `EIO`, `ENOSPC`, `ENOTDIR`,
`EISDIR`, `ENOTEMPTY`, `ENAMETOOLONG`, `ESTALE`, `ECANCELED` and
`EPROTO`. A receiver maps an unknown code to `EIO`.

### Requests

| Request | Reply |
| --- | --- |
| `LIST PATH` | `OK COUNT`, then COUNT entry lines |
| `STAT PATH` | `OK KIND SIZE MTIME` |
| `GET PATH OFFSET SIZE MTIME` | `OK`, then chunks from OFFSET, then `E` |
| `PUT PATH SIZE MTIME` | `OK OFFSET`; the client sends chunks from OFFSET and `E`; the server answers `OK` |
| `MKDIR PATH` | `OK` |
| `RENAME FROM TO` | `OK` |
| `DELETE PATH` | `OK` |
| `QUIT` | `OK`, and the server closes the connection |

An entry line of `LIST` is `KIND SIZE MTIME NAME`. KIND is `d` for a
folder and `f` for a file. SIZE is the size in bytes, 0 for a folder.
MTIME is the modification time in seconds since 1970-01-01 UTC. NAME is
the percent encoded name. The server follows symbolic links. The server
omits other kinds of files and the part files of the server.

`GET` names the size and the modification time that the client expects.
The server answers `ERR ESTALE` when the file has another size or another
modification time. An OFFSET above SIZE is `EINVAL`.

`RENAME` never replaces an existing target. It answers `EEXIST` instead.
`DELETE` removes a folder with its contents.

### Chunks

A chunk is the line `C LEN CRC` followed by LEN bytes. LEN is between 1
and 65536. CRC is the CRC-32 of the bytes (the polynomial of gzip and
zlib) as eight lowercase hexadecimal digits. The line `E` ends the data.

The receiver of a `GET` can cancel the transfer at any time with the
line `A`. The server reads input before each chunk. When the server finds
`A`, the server sends `A` instead of the next chunk. The client discards
chunks until it reads `A` or `E`. A server that has already sent `E`
receives the `A` as a request. The server ignores a request `A` and sends
no reply.

The sender of a `PUT` cancels with the line `A` instead of the next chunk.
The server answers `ERR ECANCELED`. When the server cannot read the file
during a `GET`, the server sends `X CODE MESSAGE` instead of the next
chunk. `X` ends the transfer.

A `PUT` receiver checks the CRC of each chunk before the chunk is written.
After the first bad chunk, the server reads the remaining chunks until `E`
and writes none of them. The server then answers `ERR EIO checksum
mismatch`. The client checks the chunks of a `GET` in the same way.

## Part files and resume

The receiver of a file writes it to a part file in the target folder. The
part file of `NAME` with SIZE bytes and the modification time MTIME is
named `.NAME.SIZE-MTIME.mft-part`. A complete file receives MTIME as its
modification time. The receiver then renames the part file to NAME. The
rename replaces an existing file.

A new transfer of the same file resumes from the length of its part file.
The server reports that length in the reply to `PUT`. The client sends
that length as OFFSET of a `GET`. A part file of the same NAME with
another SIZE or MTIME belongs to an older version of the file. The
receiver removes it before the transfer starts. A cancelled or failed
transfer leaves its part file for the next attempt. `LIST` omits part
files. The file views of both programs hide them as well.

## Folders

The client copies a folder by recursion. A download lists the remote
folder, creates the local folder and copies each entry. An upload creates
the remote folder with `MKDIR` and copies each entry. `EEXIST` of `MKDIR`
is no error during an upload.

## The window

Both programs show one window with these parts, from top to bottom:

- The connection bar: the fields Host and Port and the button Connect.
  The button reads Disconnect during a session.
- The server bar: the check box Serve, the field with the served folder,
  the button Choose, the field Port and a status text with the number of
  connected clients.
- Two file panes side by side: "This computer" on the left and the
  remote folder on the right. Each pane has the button Up, the path of its
  folder, a table with the columns Name, Size and Modified, and the
  buttons New folder, Rename, Delete and Refresh. The left pane also has
  the button Upload, and the right pane the button Download. A double
  click opens a folder.
- The transfer list: a table with the columns Name, Direction, Size and
  Status, a progress bar of the current file, a summary text with the
  files, the bytes and the rate, and the button Cancel.
- A status line with the last message.

A file or folder can be dragged from one pane to the other. A drop on a
folder row copies into that folder. A drop elsewhere copies into the
folder of the pane. When the target folder contains a name of the copied
items, the program asks before it replaces anything.

The minios program also accepts files dragged from Files and from the
desktop (`text/uri-list`, `dnd.md`). A drop on the remote pane uploads
them. Rows of the local pane are dragged as `text/uri-list`, so Files
accepts them as well. Tk has no protocol for drags between programs. The
host program therefore supports drags between its own panes only.

## Command line

Both programs offer the same commands besides the window:

    transfer [gui] [HOST[:PORT]]
    transfer serve [-p PORT] [DIR]
    transfer ls [-h HOST] [-p PORT] [PATH]
    transfer get [-h HOST] [-p PORT] [-o DIR] PATH...
    transfer put [-h HOST] [-p PORT] [-d PATH] FILE...
    transfer mkdir|rm [-h HOST] [-p PORT] PATH...
    transfer mv [-h HOST] [-p PORT] FROM TO

The host program is `tools/transfer.py` with the same arguments.

## Implementation

### Host (`tools/`)

- `tools/mft.py`: the protocol. `Client` performs the requests, with
  `plan_get` and `plan_put` for folders. `Server` accepts connections and
  serves each session in its own thread. `MftError` carries the POSIX
  name of an error. A local error, such as a refused connection, carries
  its own name; an error line carries only the codes of the protocol.
- `tools/transfer.py`: the command line and the Tkinter window. The
  window runs the client in a worker thread and receives its events
  through a queue, which the Tk loop reads every 50 ms. The server of the
  window runs in the threads of `Server`.
- `tools/tests/test_mft.py`: unit tests of the protocol, of resume, of
  cancellation and of the window.

### minios (`user/packages/transfer/`)

- `user/lua/lnet.c`: the module `net` of the interpreter (`lua.md`).
- `files/usr/share/lua/5.5/mft.lua`: the protocol. `mft.connect` returns
  a client with `list`, `stat`, `get`, `put`, `mkdir`, `rename`, `delete`
  and `quit`. `mft.download_plan` and `mft.upload_plan` list the steps of
  a folder copy, and `mft.run_step` performs one step.
  `mft.serve_session` answers the requests of one client, and `mft.serve`
  accepts clients and starts one worker thread `session.lua` for each.
- `files/usr/share/apps/transfer/ui.lua`: the window. It has the parts and
  the texts of the window of the host program.
- `files/usr/share/apps/transfer/client.lua`: the worker thread of the
  client. The window sends it commands and receives events through the
  queues of the `thread` module. A command that arrives during a transfer
  waits until the transfer ends, except Cancel.
- `files/usr/share/apps/transfer/server.lua`: the worker thread of the
  server of the window. It runs `mft.serve`.
- `files/usr/share/apps/transfer/session.lua`: the worker thread of one
  client of a server.
- `files/usr/bin/transfer`: the command line, a Lua script. Its commands
  print the same lines as those of the host program.
- `kernel/tests/test_transfer_gui.c`: the kernel side of `gui_transfer`.

A client waits at most 30 seconds (Lua) or 120 seconds (Python) for each
step of the server. A server waits for the next request without a limit.

## Tests

- `make check-transfer` runs the 18 unit tests of `tools/mft.py` and of
  the window of `tools/transfer.py`, the 86 checks of
  `user/packages/transfer/tests/protocol.lua` (the Lua client with the
  Python server, the Python client with the Lua server, the Lua client
  with the Lua server) and the 58 checks of `tests/window.lua`, which
  drive the window of the minios program against the Python server
  through its methods and through injected mouse and drag messages.
- The boot case `net_transfer` installs the package and runs the minios
  program against the host program in both directions, with a resumed
  download and a resumed upload, folders with an empty folder, remote
  file management, `EEXIST` and `ECONNREFUSED`.
- The boot case `gui_transfer` starts the window of the minios program in
  the compositor with a server of the guest, waits until the window has
  listed the served folder, and closes the window with Alt+F4. The window
  appends its status lines and its remote listings to the file of the
  environment variable `TRANSFER_LOG` when the variable is set. The case
  reads that file.
