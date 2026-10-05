# Lua native threads and system primitives

`require "thread"` creates native pthread workers in the interpreter process.
Each worker owns an independent Lua state, allocator activity and garbage
collector. Lua objects, closures, globals and GUI widgets are never shared.
Copied byte strings carry commands and results. This permits CPU work to run
on another virtual CPU while the main thread services the GUI.

The implementation is `user/lua/lthread.c`. Descriptor and timing operations
are in `user/lua/lposix.c`, registered as part of `require "sys"`.

## Starting and communicating with a worker

The parent starts a script file, optionally supplying up to 8192 bytes of
initial data. The child receives that string as its chunk argument and
`arg[1]`, with the script path in `arg[0]`. Its `package.path` is copied from
the parent at creation. It loads the normal standard libraries and MiniOS
modules independently.

Parent script:

```lua
local thread = require 'thread'
local worker <close> = assert(thread.spawn('/home/echo-worker.lua', 'initial'))
assert(worker:send('hello\0world'))
local reply = assert(worker:receive(2000))
assert(reply == 'hello\0world')
assert(worker:join())
```

`/home/echo-worker.lua`:

```lua
local thread = require 'thread'
local initial = ...
local message = assert(thread.receive(2000))
assert(thread.send(message))
```

Each direction has a queue limited to 128 messages and 65536 payload bytes.
An individual message may contain arbitrary bytes, including NUL, up to 8192
bytes. Sends copy the data. An oversized message is an argument error. A
send without a timeout never waits for queue space. A full queue then
returns `nil, message, EAGAIN`. A send with a timeout waits for space until
the timeout passes or a stop is requested, and then returns `EAGAIN`. A
stop request ends the wait, so a worker that sends while its parent joins it
cannot wait forever. Applications must retry, coalesce replaceable updates,
or handle the failure. The binding never silently drops a successfully
queued message.

| Parent handle | Behavior |
| --- | --- |
| `send(bytes [, timeout_ms = 0])` | Enqueue a command, returning true or `nil, message, errno` |
| `receive([timeout_ms = 0])` | Receive one result, waiting up to the timeout |
| `fd()` | Borrow the result queue's descriptor for polling or a GUI watch |
| `status()` | Return `"running"`, `"done"` or `"error"` |
| `stop()` | Request cooperative shutdown and wake both queue descriptors |
| `join()` | Wait for termination, returning true or `nil, traceback` |
| `close()` | Request shutdown, join and release the handle |

In a worker, `thread.send(bytes [, timeout_ms])`, `thread.receive` and
`thread.fd` operate on its parent connection. `thread.stop_requested()`
reads the cancellation flag. `thread.active()` counts active workers in the
process, excluding the main thread. Nested workers are allowed and need the same explicit cleanup.

A receive timeout of 0 is nonblocking, -1 waits indefinitely, and positive
values are milliseconds. No available message returns `ETIMEDOUT`. Pending
messages can still be drained after shutdown. Once the queue is empty,
shutdown or completion returns `EPIPE`. Parent sends after stop fail with
`EPIPE`. Workers can still send final results during cooperative shutdown.
Compare errno values against `sys.errno`, rather than hardcoded numbers.

A queue descriptor is level-triggered and readable while data, cancellation
or completion is pending. Drain it with `receive`, never `sys.read`, and
never close it separately. Remove its GUI watch before closing the handle.
Observe termination and remove the watch to avoid repeatedly polling a
completed queue. `join` is repeatable and `close` is idempotent.

## Ownership and shutdown

`stop` is a request, not an asynchronous interruption. A long computation
must check `thread.stop_requested()` at bounded intervals. An arbitrary
blocking native operation or an infinite loop that ignores cancellation can
prevent `join`, `close` and a Lua `<close>` scope from finishing. Pollable
operations and finite timeouts give workers a way to observe shutdown.

Returning from the worker script terminates that worker. A Lua error is
caught, its traceback is retained up to 1023 bytes, and worker-owned Lua
resources are closed before completion is published. `os.exit` in a worker
raises an error because the ordinary operation terminates the whole process.
A collected handle requests stop and detaches without blocking the garbage
collector. Explicit close/join is required when the caller must know that
resources have actually been released. Close errors are returned, so check
an explicit `close()` when its result matters.

`require "gui"` is rejected in workers. All GUI objects and callbacks belong
to the main thread. A worker can own an independent audio connection and
playback stream, then report meters to the GUI through messages.

These are threads in one process, not a security boundary. File descriptors,
current directory, environment and process lifetime are shared. Coordinate
ownership of raw descriptors and avoid concurrent changes to process-wide
state. Native modules must be safe to use from distinct Lua states. The
worker guard does not sandbox arbitrary native code or process operations.

## Descriptor and timing API

Existing `sys.open(path)` launches the MIME handler. The raw descriptor API
uses the distinct name `open_fd`.

| Operation | Result and behavior |
| --- | --- |
| `sys.open_fd(path [, flags, mode])` | Owned descriptor, defaults to read-only and mode 0666, always close-on-exec |
| `sys.nonblock(fd [, enabled])` | Query nonblocking status, or set it and return true |
| `sys.pipe([nonblocking = false])` | Owned read and write descriptors, both close-on-exec |
| `sys.write(fd, bytes)` | Bytes written, possibly fewer than supplied |
| `sys.poll(entries [, timeout_ms = -1])` | Ready-entry count, sets each entry's `revents` |
| `sys.close(fd)` | Close an owned descriptor |
| `sys.clock_ns([clock = "monotonic"])` | Integer elapsed-clock nanoseconds, or `"realtime"` for the wall clock |
| `sys.usage([scope = "process"])` | CPU and resource accounting, also accepts `"thread"` and `"children"` |
| `sys.thread_id()` | Kernel ID of the calling thread |
| `sys.crc32(bytes [, crc = 0])` | The CRC-32 of gzip and zlib, continued from `crc` |
| `sys.isatty(fd)` | True when the descriptor is a terminal |

`sys.poll` accepts up to 1024 entries of the form
`{fd = descriptor, events = sys.POLLIN | sys.POLLOUT}`. Events default to
`POLLIN`. Timeout semantics match receive, with a different default of -1.
Returned events may include `POLLERR`, `POLLHUP` and `POLLNVAL`. Errors return
`nil, message, errno`. An interrupted poll can return `EINTR`.

Open flags include `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`,
`O_APPEND`, `O_EXCL` and `O_NONBLOCK`. `sys.errno` exports `EAGAIN`, `EINTR`,
`ETIMEDOUT`, `EPIPE`, `EINVAL`, the file errors `ENOENT`, `EEXIST`,
`EACCES`, `EPERM`, `EIO`, `ENOSPC`, `ENOTDIR`, `EISDIR`, `ENOTEMPTY`,
`ENAMETOOLONG`, `EXDEV`, `ESTALE`, `ECANCELED` and `EPROTO`, and the
network errors `ECONNREFUSED`, `ECONNRESET`, `EHOSTUNREACH`, `ENETUNREACH`
and `EADDRINUSE`. The existing `sys.read(fd [, count])`
returns an empty string for would-block or interruption, and nil at EOF.
Callers needing readiness distinctions should inspect poll events first.
`sys.sleep(ms)` accepts nonnegative integer milliseconds up to INT_MAX.

Usage fields are `user_ns`, `system_ns`, their sum `cpu_ns`,
`voluntary_switches`, `involuntary_switches` and `max_rss_kb`. These wrap
`getrusage`, including `RUSAGE_THREAD`. MiniOS CPU accounting has timer-tick
resolution, regardless of the nanosecond units. Resource fields retain
kernel scope semantics and need not describe exclusively private thread
memory. Darwin host tests use Mach thread CPU accounting, which supplies
only the three CPU fields for the thread scope.

Use `clock_ns` differences for deadlines and elapsed section time, and
`usage('thread').cpu_ns` differences for CPU consumed by a thread. MiniOS
currently implements `os.clock()` using monotonic elapsed time, so it must
not be used as a CPU utilization measurement.

## Validation

`make check-lua` runs 451 binding checks and the synthesizer worker test on
the host. `lua_threads` runs the binding checks inside MiniOS, covering
state isolation, thread IDs, binary messages, queue limits, descriptor
readiness, timeout and stop behavior, worker errors, GC, simultaneous workers,
and CPU accounting that excludes sleep. `luasynth_worker` proves that audio
advances while the GUI thread sleeps, and that close joins the worker.
The `pthreads` regression also checks aligned SIMD stack access and concurrent
stream open/flush/close operations required by native Lua workers.
