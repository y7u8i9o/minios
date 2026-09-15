# Lua

Lua 5.5.1 runs on minios as `/bin/lua` and `/bin/luac`. The interpreter
sources are vendored unchanged; the port consists of the libc functions Lua
needed, a build rule and a boot test. This document records the state of
the port and the work that remains.

## Sources

`third_party/lua/` holds the `src/` directory of the release archive, the
manual and the manual pages from `doc/`, the upstream `README`, the MIT
license in `LICENSE` and the release, download address and SHA-256 in
`NOTICE`. Nothing in `src/` is edited: a later release is dropped in
by replacing the directory. The vendored files are exempt from the minios
line count and naming conventions.

## Configuration

`luaconf.h` is left as shipped. `user/Makefile` makes the platform choices
on the compiler command line:

- `LUA_USE_POSIX`. Selects `_setjmp`/`_longjmp` for error unwinding,
  `getc_unlocked` with `flockfile` for `io.read`, `fseeko`/`ftello`,
  `mkstemp` for `os.tmpname`, `popen`/`pclose` for `io.popen`,
  `gmtime_r`/`localtime_r`, `isatty` for the prompt decision, `sigaction`
  for the interrupt handler, and the `sys/wait.h` macros that turn the
  status of `os.execute` and `io.close` into `exit`/`signal` and a code.
  `LUA_USE_DLOPEN` and `LUA_USE_READLINE` stay off.
- `LUA_PATH_DEFAULT` is `/usr/share/lua/5.5/?.lua;/usr/share/lua/5.5/?/init.lua;./?.lua;./?/init.lua`
  and `LUA_CPATH_DEFAULT` is empty: `require` finds Lua modules in the
  share tree and the current directory and there is no dynamic loading.
- Numbers are the defaults, 64-bit integers and `double`. User programs
  compile with SSE2 and the kernel saves the FXSAVE area per thread, so
  floating point needs no further support (see `floating.md`).

The core and library objects are compiled to `build/user/lua/` with
dependency files and linked twice, with `lua.c` and with `luac.c`.

## libc additions

Compiling the unmodified sources against libc reported what was missing.
Everything below was added for the port and is available to every
program:

- `signal.h`: `sig_atomic_t`.
- `ctype.h`: `isgraph`, `isblank`.
- `string.h`: `strcoll`, which is `strcmp` because the sole locale is C.
- `setjmp.h`: `_setjmp` and `_longjmp`, aliases of `setjmp` and `longjmp`
  in `setjmp.S` because there is no signal mask to save.
- `stdio.h`: `ungetc` (one byte in the read window; a push back onto an
  empty window shifts it), `freopen` (the standard streams keep their
  identity), `tmpfile` (a file under `/tmp` that `fclose` removes; the
  stream remembers the path), `tmpnam` with `L_tmpnam`, `TMP_MAX`,
  `P_tmpdir` and `FILENAME_MAX`, `popen` and `pclose` (a pipe to
  `/bin/sh -c`; the stream remembers the child pid), `getc_unlocked`,
  `flockfile`, `funlockfile`, `fseeko` and `ftello`.
- `stdlib.h`: `system` (`/bin/sh -c`, interrupt and quit ignored in the
  parent while the child runs; a null command reports that a shell
  exists) and `mkstemp`.
- The root image gains an empty `/tmp` directory.

`tests/cases/libc_ext` covers each of these functions directly.

## Modules

The interpreter links libgui, libaudio, libwire, libfont and libedit.
`user/lua/linit.c` replaces the upstream `linit.c` (the upstream file
states that it may be replaced): it opens the standard libraries under
the same selection masks and registers the minios modules in
`package.preload`, so `require "fs"` works without a file search.
The objects of `user/lua/` are named `minios_*.o` so that the pattern
rule for the upstream sources does not produce them. Failures in the
modules return `nil`, the message and the errno, as `io.open` does.

`fs` (`user/lua/lfs.c`) provides `dir(path)`, an iterator over name and
type (`file`, `directory`, `char`, `block`, `fifo`, `unknown`) that
skips `.` and `..`; `list(path)`, a sorted array of names; `stat(path)`
with `type`, `size`, `mtime`, `mode`, `inode`, `device`, `links`,
`blocks`, `blocksize`; `exists`; `mkdir(path [, mode])`, `rmdir`,
`chdir`, `getcwd`, `sync`; `read(path)` for a whole file and
`write(path, data [, append])`.

`sys` (`user/lua/lsys.c`) provides `spawn(program, args...)`, which
starts a program as a child of init through `mime_spawn`; `run(program,
args...)`, which waits and returns `true` or `nil`, `"exit"` or
`"signal"`, and the number; `open(path)`, which starts the program
registered for the file type or the exec line of a launcher;
`type(path)`, `handler(type)` and `mime_load(types, apps)`; `pid`,
`ppid`, `kill(pid [, signal])` with a number or a name such as `"TERM"`;
`sleep(ms)`, `uptime()` in milliseconds, `yield`; `uname()` as a table,
`nproc` and `cpu`.

Native workers are available through `require "thread"`. The `sys` module
also supplies raw descriptor I/O, polling, monotonic nanosecond clocks,
thread IDs and per-thread CPU accounting. See [Lua native threads and system
primitives](lua-threads.md) for API contracts, examples and ownership rules.
The GUI remains on the main thread, while workers own separate Lua states.

## The gui module

`gui` (`user/lua/lgui.c`, `user/lua/lpaint.c`) binds the application
framework of libgui (`framework.md`).

`gui.app()` connects to the display server and returns the application;
it fails with `nil` and a message without a server. The application has
`window(w, h, title)`, `modal(parent, w, h, title)`, `run()`, `quit(code)`,
`step(timeout_ms)`, `timer(ms, repeat, fn)` (the timer has `remove()`),
`watch(fd, "r"|"w"|"rw", fn)` (the watch has `remove()`; `fn(fd, ready)`),
`theme()` (tables `color` and `metric` by name, `scale`), `dialog(title,
text, {buttons})` returning the index of the button, `prompt(title,
label, default)` returning the text or `nil`, and `destroy()`.

Constructors take the parent first: `box(parent, vertical)`, `vbox`,
`hbox`, `grid`, `label(parent, text)`, `button`, `checkbox`, `radio`,
`textfield`, `canvas`, `separator`, `listview`, `scrollbar(parent,
vertical)`, `scrollarea`, `combobox`, `spinner(parent, min, max,
value)`, `slider`, `progress`, `tabs` and `splitpane(parent, vertical)`.
Every widget is a userdata with the methods `on(signal, fn)`, `text`,
`value`, `range`, `visible`, `enabled`, `hint`, `min`, `max`, `stretch`,
`align` (`fill`, `start`, `center`, `end`), `grid(row, col, rowspan,
colspan)`, `gridstretch`, `accel`, `padding`, `tip`, `id`, `find`,
`class`, `invalidate`, `relayout`, `focus`, `focused`, `capture`, `size`,
`pos`, `parent`, `window`, `destroy`; windows have `close` and `title`;
list views and combo boxes have `add`, `clear`, `count`, `item` and
`select` (indexes start at 1); tabs have `page(title)` and `select`;
split panes `position`; scroll bars `set(value, max, page)`. Setters
return the widget, so calls chain.

A handler is `fn(widget, args)`; a true result consumes the signal. The
argument tables follow the libgui structures: `clicked`, `press`, `motion`, `release` and `wheel` have
`button`, `x`, `y`; `key` and `keyup` have `code`, `ch`, `mods` and `char`;
`changed`, `toggled`, `activate` and `focus` have `value` and `text`,
except on combo boxes, tabs and list views where they have `index`;
`selected` has `index`; `scrolled` has `value`; `resize` has `w` and
`h`; `close` has none. `paint` receives a painter with `fill`, `frame`,
`line`, `rounded`, `text`, `text_width`, `text_height`, `push`, `pop`,
`focus_ring` and `clip`; the painter is valid during the handler only
and raises an error afterwards. Colours are integers `0xRRGGBB`;
`gui.rgb(r, g, b)` builds one. `gui.key` names the key codes (`esc`,
`enter`, the arrows, `f1` to `f12`, letters and digits) and `gui.mod`
the modifiers.

The registry maps each C widget to one userdata, so a widget returned
twice is the same object. libgui emits `destroy` when a widget is freed
(`widget_destroy`), which clears the pointer; a method on a destroyed
widget raises "widget was destroyed". Errors inside handlers are printed
with a traceback to stderr and the program continues. `gui.test` holds
`key`, `mouse`, `close`, `paint` and `pixel`, which inject messages into
a window and read its surface for tests.

The Pong package includes its Lua version at
`/home/.local/share/apps/pong.lua`.

`.lua` has the MIME type `text/x-lua`; installing the Code package
registers its editor as the handler.
A launcher file starts a script with `exec=/bin/lua /home/.local/share/apps/name.lua`;
`mime_open` passes one argument after the program.

## Additions for the Code editor

The editor (`code.md`) added the constructors `editor`, `menubar`, `menu`,
`popupmenu`, `toolbar`, `statusbar`, `treeview` and `table`, the editor,
menu, tool bar, status bar, icon and data view methods, models written in
Lua, the `context` signal, and `sys.spawn_pipe`, `sys.read`, `sys.close`
and `sys.wait`. `code.md` lists them. `editor:font(path [, px])` loads an
outline font for one editor (`editor_set_font` in libgui); `px` defaults
to 13 logical pixels and `nil` returns to the theme's font.

## Audio

`require "audio"` loads the binding in `user/lua/laudio.c`. It uses
`libaudio` and the same `audiod` server as native applications. The
module can be loaded without an audio device or server; `audio.connect()`
returns an error when the server is unavailable. A desktop session starts
`audiod` automatically. A console session can start it with `audiod &`.

The module constants describe the fixed stream format: `audio.rate` is
48000, `audio.channels` is 2, `audio.frame_bytes` is 4, and `audio.format`
is `"s16le"`. PCM data is a binary Lua string of interleaved signed
16-bit little-endian left/right samples. For example,
`string.pack("<i2i2", left, right)` produces one frame. The binding does
not decode WAV files, resample, or normalize floating-point samples.

| Object | Methods |
|---|---|
| `audio` | `connect()` returns a connection |
| connection | `playback([name])`, `capture([name [, source]])`, `mixer()`, `fd()`, `dispatch([timeout_ms])`, `sync()`, `close()` |
| playback | `write(pcm)`, `start()`, `pause()`, `drain()`, `volume(percent)`, `ready()`, `info()`, `close()` |
| capture | `read([frames])`, `start()`, `stop()`, `volume(percent)`, `available()`, `info()`, `close()` |
| mixer | `streams()`, `master([percent])`, `volume(id, percent)`, `generation()`, `sync()`, `close()` |

Stream names default to `"Lua playback"` and `"Lua capture"`; an explicit
name has at most 47 bytes and no embedded NUL. Capture source is `"input"`
(the default) or `"monitor"` (the output mix). All volume percentages
range from 0 to 200, with 100 meaning unity gain. Stream creation leaves
playback and capture paused. `drain()` starts playback if necessary and
waits for the queued samples to finish.

`write` returns the number of **frames** accepted, not the number of
bytes. Its string must contain complete four-byte frames. `read`
returns a PCM string and its frame count; it defaults to one quantum.
Both calls can block and can return a partial transfer. A caller must
handle the returned count; after writing `n` frames, the unwritten part
of the string begins at `n * audio.frame_bytes + 1`. A zero-size transfer
returns zero frames (and an empty string for capture).

`playback:ready()` reports frames that can be submitted without waiting,
while `capture:available()` reports frames already captured. These are
cached counts updated by dispatch. Each playback buffer holds one
quantum; even a short write consumes a buffer. Start playback before a
write larger than the ready capacity, otherwise a paused stream cannot
release buffers. For GUI work, use `app:watch(connection:fd(), "r", fn)`
to call `connection:dispatch(0)` and transfer only ready/available frames.
Prime the playback buffers before waiting for an event. Remove the watch
before closing its connection; the descriptor belongs to the connection
and must not be closed separately with `sys.close`.

`dispatch` returns the number of events dispatched, or zero after its
timeout; the default timeout is zero, and -1 waits indefinitely. `sync`
waits until the server has processed preceding requests. Use a round trip
before examining the result of an asynchronous state or volume change.
`info()` returns a table with `rate`, `channels`, `quantum` (frames),
`xruns`, `error` (an errno number, zero on success), and `state` (`"paused"`,
`"running"`, or `"error"`).

`mixer:streams()` returns a new array of snapshots, each holding `id`,
`name`, `direction` (`"playback"` or `"capture"`), `volume`, `state` and
`peak` (0 to 32767). Later events do not mutate a returned table. The
mixer sees streams from every client. `master()` reads its cached master
volume; `master(percent)` changes it, and `volume(id, percent)` changes
one stream. Call `mixer:sync()` to refresh the view after changes;
`generation()` is its change counter.

Operations that return no data return `true` on success. Audio failures
return `nil, message, errno`, like `io.open`. Invalid arguments and use of
closed objects raise Lua errors. `close()` is the exception: it returns
no values and is idempotent. All four object types support `__gc` and
Lua's `<close>` variables. A stream or mixer keeps its connection alive;
closing a connection destroys its native children and makes subsequent
operations on their Lua objects fail safely. Explicitly close or scope
objects when timely release matters.

A tone example is installed as `/usr/share/lua/examples/tone.lua`:

    lua /usr/share/lua/examples/tone.lua 440 0.5

It synthesizes PCM in quantum-sized blocks, handles partial writes, and
drains before its scoped playback and connection objects close.

The separate `luasynth` package uses this binding for an eight-voice
synthesizer with two oscillators, modulation, delay and saved presets.
`luasynth.md` describes it; the C synthesizer remains available as `synth`.

## Tests

`user/etc/tests/modules.lua` checks every function of `fs` and `sys`.
`make check-lua` (part of `make check`) compiles the interpreter,
`user/lua/` and `libgui/src/mime.c` with the host compiler and runs the
script on a scratch directory with the MIME tables from `user/etc/`.
`user/lua/tests/gui.lua` runs on the host only, over the fake client
of libgui: layout, signals, painting and pixels, the painter lifetime,
close handling, destroyed widgets, timers and the constructors.
`user/lua/tests/audio.lua` checks PCM conversion, partial transfers,
argument bounds, injected audio errors, snapshots, garbage collection,
connection retention and explicit/scoped close against
`user/lua/tests/fake_audio.c`. This backend is linked only into the host
test program, never the MiniOS interpreter.
`tests/cases/lua_audio` starts a real `audiod` on QEMU's silent capture
backend: it verifies Lua-generated stereo PCM sample by sample through
the monitor, reads input in partial buffers, changes mixer controls, and
checks native stream removal and closing a connection before its children.
The test driver reaps both Lua and audiod even when an assertion fails.
`tests/cases/gui_lua` starts `/etc/tests/luagui.lua` on the compositor,
finds the colour of its canvas on the screen and closes it with Escape.
`tests/cases/lua_sys` runs the same script on minios; it passes
`--no-init`, which skips the `spawn` check because the kernel run test
starts the program without init and a child of init would stay a
zombie and count as leaked pages.

`tests/cases/lua_conf` runs `/etc/tests/conformance.lua`, twelve
sections of language and library checks (integers and bit operations,
floats, strings, patterns, tables, closures, metatables, coroutines,
error handling, garbage collection, file I/O, `os`); each prints `PASS`.
It found `pow` one unit low, which `math.c` now computes with the x87
kernels, and the quadratic allocator described in `libc.md`.
`tests/cases/lua_gc` runs `/etc/tests/gcbench.lua`, which allocates up
to 40000 objects and collects them; the 60 second timeout fails the case
if allocation becomes quadratic again.

`tests/cases/lua` runs `lua /etc/tests/test.lua first`. The script
exercises recursion, integer and float arithmetic and formatting
including `%a`, string methods, collation, `utf8`, tables and sorting,
`arg`, `pcall`, coroutines, a temporary file read back with `read("n")`
(which relies on `ungetc`), `seek`, `os.tmpname`, `io.lines`,
`os.remove`, loading a `string.dump` chunk from a file (which relies on
`freopen`), `os.clock`, `os.time`, `os.date`, `os.execute` with its exit
status, and `os.getenv`. The expected output pins every printed line.

## Remaining work

- Interactive use. `lua` without arguments reads lines with `fgets` in
  canonical mode, so the prompt works but has no history or editing.
  `lua.c` provides the `lua_readline`, `lua_saveline` and `lua_initreadline`
  hooks for a replacement; wiring them to `libedit` needs a small file
  outside `third_party/` that the Makefile compiles into the interpreter.
- Manual pages. `third_party/lua/doc/lua.1` and `luac.1` are troff; the
  minios `man` shows plain text, so pages in `user/share/man/man1/`
  have to be written in that format.
- Module directory. `/usr/share/lua/5.5/` does not exist yet; it is
  created with the first Lua module.
- Bindings not yet in `gui`: images beyond the named icons, and the
  clipboard, and layer windows.
- `os.setlocale` accepts only `C`, `POSIX` and the empty string, and
  `os.date` reports UTC because `localtime` is `gmtime`.
- `LUA_INIT` and the `-E`/`-W` options work as upstream; nothing sets
  `LUA_INIT` in the profile.
