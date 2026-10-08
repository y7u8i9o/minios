# Lua

Lua 5.5.1 runs on minios as `/bin/lua` and `/bin/luac`. The interpreter
sources are vendored unchanged; the port consists of the libc functions Lua
needed, a build rule and a boot test. This document records the state of
the port and the work that remains.

## Sources

`third_party/lua/` contains the `src/` directory of the release archive, the
manual and the manual pages from `doc/`, the upstream `README`, the MIT
license in `LICENSE` and the release, download address and SHA-256 in
`NOTICE`. Nothing in `src/` is edited: a later release is dropped in
by replacing the directory. The vendored files are exempt from the minios
line count and naming conventions. The upstream manual pages are troff,
which the minios `man` does not read, so `user/share/man/man1/lua.1` and
`luac.1` restate them as plain text, together with the line editing
keys, the module directory and the history file of minios.

## Configuration

`luaconf.h` is left as shipped. `user/Makefile` makes the platform choices
on the compiler command line:

- `LUA_USE_POSIX`. Selects `_setjmp`/`_longjmp` for error unwinding,
  `getc_unlocked` with `flockfile` for `io.read`, `fseeko`/`ftello`,
  `mkstemp` for `os.tmpname`, `popen`/`pclose` for `io.popen`,
  `gmtime_r`/`localtime_r`, `isatty` for the prompt decision, `sigaction`
  for the interrupt handler, and the `sys/wait.h` macros that turn the
  status of `os.execute` and `io.close` into `exit`/`signal` and a code.
  `LUA_USE_READLINE` remains off. The line editing of the prompt comes
  from `user/lua/lreadline.h` instead (see "The interactive prompt"
  below).
- `LUA_USE_DLOPEN` is on since P2 of `docs/plan/packaging.md`, which
  lets `require` load C modules with `dlopen`.
- `LUA_PATH_DEFAULT` is `/usr/share/lua/5.5/?.lua;/usr/share/lua/5.5/?/init.lua;./?.lua;./?/init.lua`
  and `LUA_CPATH_DEFAULT` is `/usr/lib/lua/5.5/?.so`. `require` finds Lua
  modules in the share tree and the current directory, and C modules in
  `/usr/lib/lua/5.5`. The share tree rule of `user/Makefile` creates
  `/usr/share/lua/5.5/`, which is empty until a module is installed
  there.
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
- `string.h`: `strcoll`, which follows LC_COLLATE (`locale.md`).
- `setjmp.h`: `_setjmp` and `_longjmp`, aliases of `setjmp` and `longjmp`
  in `setjmp.S` because there is no signal mask to save.
- `stdio.h`: `ungetc` (one byte in the read window; a push back onto an
  empty window shifts it), `freopen` (the standard streams retain their
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

The interpreter links liblua and libc, and libedit statically for its
prompt. `user/lua/linit.c` replaces the upstream `linit.c`, which the
upstream file permits. It opens the standard libraries under the same
selection masks and registers the modules of the interpreter, `fs`,
`sys`, `thread` and `net`, in `package.preload`, where `require "fs"` finds
them without a file search. `gui`, `audio` and `mime` are C modules,
`/usr/lib/lua/5.5/gui.so`, `audio.so` and `mime.so`, which `require`
loads through `package.cpath`. Each links liblua and its own library,
`gui.so` libgui, `audio.so` libaudio and `mime.so` libgui, and none uses
a symbol of the interpreter program. The two helpers they share,
`minios_errresult` and `minios_lua_worker`, are inline functions of
`user/lua/minios.h`, and a worker state is marked by the registry field
`minios.worker`. Each module is therefore a library like any other for
the library rule of `pkg`, and `lua` with its modules can form the
packages `lua`, `lua-gui` and `lua-audio`. Before P2 the interpreter
linked libgui, libaudio, libwire and libfont and preloaded `gui` and
`audio` as well. The objects of `user/lua/` are named `minios_*.o` so
that the pattern rule for the upstream sources does not produce them.
Failures in the modules return `nil`, the message and the errno, as
`io.open` does.

`fs` (`user/lua/lfs.c`) provides `dir(path)`, an iterator over name and
type (`file`, `directory`, `link`, `char`, `block`, `fifo`, `unknown`)
that skips `.` and `..`; `list(path)`, a sorted array of names;
`stat(path)` with `type`, `size`, `mtime`, `mode`, `inode`, `device`,
`links`, `blocks`, `blocksize`; `lstat(path)`, the same for a symbolic
link itself; `exists`; `mkdir(path [, mode])`, `rmdir`,
`chdir`, `getcwd`, `sync`; `read(path)` for a whole file and
`write(path, data [, append])`; `utime(path, mtime)`, which sets the
modification time in seconds since 1970.

`net` (`user/lua/lnet.c`) provides IPv4 TCP sockets as plain descriptors,
which `sys.close` closes and `sys.poll` and `app:watch` wait for:
`connect(host, port [, timeout_ms])`, `listen(port [, address [,
backlog]])` returning the descriptor and the bound port (port 0 selects a
free port), `accept(fd [, timeout_ms])` returning the descriptor, the
address and the port of the client, `send(fd, data [, timeout_ms])`,
which sends all of the data, `recv(fd, max [, timeout_ms])`, which returns
1 to `max` bytes or `nil` alone at the end of the stream,
`shutdown(fd [, "r" | "w" | "rw"])`, `peer(fd)` and `address(fd)`. A
timeout of -1 waits without limit, and an expired timeout returns the
errno `ETIMEDOUT`. Every socket is close-on-exec. The file transfer
program (`filetransfer.md`) uses the module in its worker threads.

`sys` (`user/lua/lsys.c`) provides `spawn(program, args...)`, which
starts a program as a child of init through an intermediate child, with
the language of the desktop settings, and `run(program, args...)`,
which waits and returns `true` or `nil`, `"exit"` or `"signal"`, and the
number. It also provides `pid`, `ppid`, `kill(pid [, signal])` with a
number or a name such as `"TERM"`, `sleep(ms)`, `uptime()` in
milliseconds, `yield`, `uname()` as a table, `nproc` and `cpu`.

`mime` (`user/lua/lmime.c`) provides `open(path)`, which starts the
program registered for the file type or the exec line of a launcher,
`type(path)`, `handler(type)` and `load(types, apps)`. These functions
were `sys.open`, `sys.type`, `sys.handler` and `sys.mime_load` until P2.
They form a module of their own, not a part of `gui`, because a worker
thread may use them and `gui` refuses to load in a worker.

Native workers are available through `require "thread"`. The `sys` module
also supplies raw descriptor I/O, polling, monotonic nanosecond clocks,
thread IDs and per-thread CPU accounting. See [Lua native threads and system
primitives](lua-threads.md) for API contracts, examples and ownership rules.
The GUI remains on the main thread, while workers own separate Lua states.

## The gui module

`gui` (`user/lua/lgui.c`, `user/lua/lpaint.c`, `user/lua/limage.c`)
binds the application framework of libgui (`framework.md`).

`gui.app()` connects to the display server and returns the application;
it fails with `nil` and a message without a server. The application has
`window(w, h, title)`, `modal(parent, w, h, title)`, `run()`, `quit(code)`,
`step(timeout_ms)`, `timer(ms, repeat, fn)` (the timer has `remove()`),
`watch(fd, "r"|"w"|"rw", fn)` (the watch has `remove()`; `fn(fd, ready)`),
`theme()` (tables `color` and `metric` by name, `scale`), `dialog(title,
text, {buttons})` returning the index of the button, `prompt(title,
label, default)` returning the text or `nil`, `open_file(title, path [,
filters])` and `save_file(...)` running the file chooser of
`folderview.md` with filters as an array of `{name, patterns}` and
returning the chosen path or `nil`, `choose_folder([title [, path]])`
running the chooser in its folder mode, `layer(w, h, options)`,
`screen()`, `clipboard([text])` (the three are described below) and
`destroy()`.

Constructors take the parent first: `box(parent, vertical)`, `vbox`,
`hbox`, `grid`, `label(parent, text)`, `button`, `checkbox`, `radio`,
`textfield`, `canvas`, `separator`, `listview`, `scrollbar(parent,
vertical)`, `scrollarea`, `combobox`, `spinner(parent, min, max,
value)`, `slider`, `progress`, `tabs`, `splitpane(parent, vertical)` and
`imageview(parent, img)`.
Every widget is a userdata with the methods `on(signal, fn)`, `text`,
`value`, `range`, `visible`, `enabled`, `hint`, `min`, `max`, `stretch`,
`align` (`fill`, `start`, `center`, `end`), `grid(row, col, rowspan,
colspan)`, `gridstretch`, `accel`, `padding`, `tip`, `id`, `find`,
`class`, `invalidate`, `relayout`, `focus`, `focused`, `capture`, `size`,
`pos`, `abs` (the position in the window), `parent`, `window`, `destroy`; windows have `close` and `title`;
list views and combo boxes have `add`, `clear`, `count`, `item` and
`select` (indexes start at 1); tabs have `page(title)` and `select`;
split panes `position`; scroll bars `set(value, max, page)`; image
views, labels and buttons `image(img)`. Setters return the widget, so
calls chain.

A handler is `fn(widget, args)`; a true result consumes the signal. The
argument tables follow the libgui structures: `clicked`, `press`, `motion`, `release` and `wheel` have
`button`, `x`, `y`; `key` and `keyup` have `code`, `ch`, `mods` and `char`;
`changed`, `toggled`, `activate` and `focus` have `value` and `text`,
except on combo boxes, tabs and list views where they have `index`;
`selected` has `index`; `scrolled` has `value`; `resize` has `w` and
`h`; `close` has none. `paint` receives a painter with `fill`, `frame`,
`line`, `rounded`, `text`, `text_width`, `text_height`, `push`, `pop`,
`focus_ring`, `clip` and `image`; the painter is valid during the
handler only and raises an error afterwards. Colours are integers `0xRRGGBB`;
`gui.rgb(r, g, b)` builds one. `gui.key` names the key codes (`esc`,
`enter`, the arrows, `f1` to `f12`, letters and digits) and `gui.mod`
the modifiers.

The registry maps each C widget to one userdata, so a widget returned
twice is the same object. libgui emits `destroy` when a widget is freed
(`widget_destroy`), which clears the pointer; a method on a destroyed
widget raises "widget was destroyed". Errors inside handlers are printed
with a traceback to stderr and the program continues. `gui.test` contains
`key`, `mouse`, `close`, `paint` and `pixel`, which inject messages into
a window and read its surface for tests. The host build adds `drag(win,
x, y, offers [, actions])`, which returns the answer of the window to a
drag that offers the MIME types of the list, `drop(win, x, y, mime,
data, action)`, `dragged()`, which returns the number of drags that the
program started, the items of the last one and its actions, and
`drag_end(win, action)`.

Tables, tree views and canvases emit `drag_begin`, `drag_motion`, `drop`,
`drag_leave` and `drag_end` (`dnd.md`) with a table of `row`, `x`, `y`,
`actions`, `action`, `mime` and `data`. A `drag_motion` handler that
takes the drag sets `accept` to the MIME type, `actions` and `preferred`
to masks of `gui.DND_COPY` and `gui.DND_MOVE`, and optionally `row`, the
row to outline (-1 for the whole view), in that table and returns true.
`gui.offers(mime)` tells whether the drag over the window offers a type.
`view:drag(items, actions [, label [, icon]])` starts a drag from a
`drag_begin` handler; `items` is a list of `{mime, data}` pairs, the
preferred type first. `view:rowrect(row)` returns the rectangle of a
visible row in the coordinates of the view.

The Pong package includes its Lua version at
`/usr/share/apps/pong.lua`.

`.lua` has the MIME type `text/x-lua`; installing the Code package
registers its editor as the handler.
A launcher file starts a script with `exec=/bin/lua /usr/share/apps/name.lua`;
`mime_open` passes one argument after the program.

## Images, the clipboard and layer windows

`user/lua/limage.c` binds the images of libgui (`gui/image.h`).
`gui.image(path [, size [, color]])` loads a PNG file at its own size,
or an SVG file, recognised by the `.svg` suffix, rendered `size` by
`size` logical pixels (16 by default, at most 1024 device pixels). An
SVG image is rendered at the scale of the first output, the way the
icon cache renders icons, so it remains sharp on a high density display;
`color` fills the paths that name no fill and defaults to black. A
missing file returns `nil`, `"path: No such file or directory"` and
`ENOENT`; a file that does not decode returns `nil`, `"path: not a
valid PNG file"` (or SVG) and `EINVAL`. `gui.from_pixels(w, h [,
data])` builds an image from a string of `w * h` pixels of four bytes,
each `0xAARRGGBB` in little endian order (`string.pack("<I4", argb)`),
row by row; without `data` the image is transparent. Sides are limited
to 16384 pixels, and a string of the wrong length is an argument error.

An image has `size()`, its width and height in logical pixels (the size
the painter draws), `scale()`, the device pixels per logical pixel (1
for PNG files and pixel strings), `pixel(x, y [, argb])`, which reads a
device pixel as `0xAARRGGBB` or stores one and returns the image, and
`pixels()`, every device pixel in the format of `from_pixels`. Pixel
values carry the alpha in the top byte, so opaque red is `0xffff0000`;
coordinates outside the image raise an error. The pixels belong to the
userdata and only the garbage collector frees them. A widget that shows
an image retains its userdata in the widget's handler table, so the
pixels outlive every widget that points at them.

The painter gained `image(img, x, y [, w, h])`. Without a size the image
is drawn at its logical size, and libgui's `painter_image` resamples by
nearest pixel when the image's scale differs from the window's. With a
size the binding resamples the image to `w * scale` by `h * scale`
device pixels with a box filter over premultiplied samples (each target
pixel averages the source pixels its area covers, which is the nearest
pixel when enlarging) and retains that rendition in the image until a
different size, scale or a `pixel` store replaces it. A size of zero
draws nothing, and a side beyond 16384 device pixels raises an error.

The class `imageview` comes from libgui (`imageview_new`).
`gui.imageview(parent [, img])` shows the image centred,
reduced to fit its area with the proportions retained and never enlarged,
on the window colour, and then emits `paint`, so a handler can draw
over it. Its preferred size is the image's logical size.
`widget:image(img | nil)` replaces the image of an image view, or sets
the icon of a label or button, which draw it beside their caption; any
other widget raises an error. `widget:icon(name)` releases an image set
this way.

`app:clipboard()` returns the text of the clipboard and
`app:clipboard(text)` makes `text` the selection and returns the
application; the text contains any bytes up to 65536. Both go through
`gui_clipboard_get` and `gui_clipboard_set` of libgui, which complete
the transfer through the data device before they return. The read
passes a pipe to the offer and waits until the owner (or the
compositor's stored copy) has written it, at most about two seconds.
The binding is therefore synchronous and needs no callback. The
compositor accepts a selection only with the serial of an input event
the client received, so a program sets the clipboard from an input
handler or after its window gained the keyboard focus (the window's
`focus` signal with `value` 1); a selection set earlier is silently
ignored by X12. A read without any selection returns `nil`, `"the
clipboard contains no text"` and `ENOENT`. libgui exposes neither a
primary selection nor drag and drop, so the module has neither.

`app:layer(w, h [, options])` creates a window on a layer surface
(`app_layer_window`), without decorations, for panels, docks and
overlays. The options table contains `layer` (`"background"`, `"bottom"`,
`"top"`, the default, or `"overlay"`), `anchor` (edge names separated
by spaces or commas from `top`, `bottom`, `left` and `right`; none by
default), `exclusive` (the exclusive zone in logical pixels, 0 by
default), `keyboard` (true asks for key events) and `namespace` (a name
for the compositor's log, `"lua"` by default). A size of 0 takes the
free desktop area in that dimension; the compositor's configure sets
the final size before `layer` returns, so `size()` reports it. The
window has the methods of any window and `close` destroys the surface.
The layer protocol has no margins, so neither the module nor libgui
offers them. `app:screen()` returns the desktop's width and height in
logical pixels, which a panel uses to lay out its contents. These lines
put a short bar along the top edge.

    local bar = assert(app:layer(0, 28, { anchor = "top left right", exclusive = 28 }))
    gui.label(bar, "status")

## The interactive prompt

`lua.c` calls `lua_initreadline`, `lua_readline`, `lua_saveline` and
`lua_freeline`, and defines its own versions only when `lua_readline`
is not defined. `user/Makefile` compiles `lua.c` with `-include
lua/lreadline.h`, whose macros map the four hooks to
`user/lua/lreadline.c`, so the vendored file remains unmodified. The glue
opens a libedit editor on the terminal at the first prompt and gives the
prompt the editing keys of the shell, which move the cursor, recall the
history with the arrows and Ctrl+R, and complete with Tab global names
and table fields after a dotted chain (`string.fo` offers
`string.format`). The
completion reads tables with raw access, so no metamethod runs while a
line is edited. Ctrl+C discards the line being edited, and Ctrl+D on an
empty line ends the session as the end of input does. lua.c saves whole
statements; the glue adds each of their lines to the history on its
own, so that recalling a line edits one line. The history contains 500
lines, is read from `$HOME/.lua_history` at the first prompt and is
written back at exit. When the editor cannot be opened, the prompt
falls back to `fgets`. The host test program replaces `lua.c` and
compiles none of this.

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
cached counts updated by dispatch. Each playback buffer contains one
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

`mixer:streams()` returns a new array of snapshots, each containing `id`,
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
Lua's `<close>` variables. A stream or mixer maintains its connection;
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

`user/etc/tests/modules.lua` checks every function of `fs`, `sys` and
`mime`. `make check-lua` (part of `make check`) compiles the interpreter,
`user/lua/` and `lib/libgui/src/mime.c` with the host compiler and runs the
script on a scratch directory with the MIME tables from `user/etc/`. The
host program links the C modules in, and `linit.c` compiled for the host
registers them in `package.preload`, where worker states find them as
well.
`user/lua/tests/gui.lua` runs on the host only, over the fake client
of libgui: layout, signals, painting and pixels, the painter lifetime,
close handling, destroyed widgets, timers and the constructors. It also
loads `lib/libgui/tests/data/rgba.png` and `shape.svg` and compares their
sizes and pixels with the formulas of `genicons.py`, checks the missing
and malformed file results, `from_pixels` and pixel stores, reads back
images drawn on a canvas at their size, enlarged and reduced, checks
the image view's placement and fitting and that a label retains its image
through a garbage collection, round trips the fake clipboard, and
creates, sizes and closes layer windows, with the errors of bad layer
names, anchors and sizes.
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
`tests/cases/gui_lua_bindings` starts `/etc/tests/luabind.lua`, which
loads a PNG and an SVG icon, checks the missing and malformed file
results, reads back an image enlarged over its canvas and an icon drawn
at twice its size, sets the clipboard once its window has the focus and
starts `/etc/tests/luaclip.lua` as a second client, which reads that
text and sets its own; the first client reads it back after the second
closed its window and exited. It then opens a layer window along the
top edge and checks that it spans the screen. The kernel test finds the
enlarged image and the layer's colour on the screen and presses Escape,
which closes the layer window and ends the program. Its first run found
a use after free in X12: an offer made to the client that received the
focus when the owner's window closed still pointed at the owner's
source after the owner disconnected. `source_gone` in
`user/compositor/data.c` now moves such offers to the stored copy.
`tests/cases/lua_prompt` types into the interactive prompt on the
console: an expression, a Tab completion of `string.up`, a history
recall with Up, a line discarded with Ctrl+C and Ctrl+D; then
`/etc/tests/luaprompt.lua` checks `$HOME/.lua_history` and the module
directory.
`tests/cases/lua_sys` runs the same script on minios; it passes
`--no-init`, which skips the `spawn` check because the kernel run test
starts the program without init and a child of init would remain a
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

- The clipboard binding is synchronous, because libgui completes each
  transfer inside `gui_clipboard_get`. An asynchronous read with a
  callback in the event loop needs a libgui call that hands out the
  pipe of the current offer, which the binding would then watch with
  `app_watch_fd`.
- libgui exposes no primary selection and no drag and drop, and the
  layer protocol has no margins, so the module offers none of them.
- `os.setlocale` accepts the locale names of `locale.md`, and `os.date`
  reports the local time of `time.md`.
- `LUA_INIT` and the `-E`/`-W` options work as upstream; nothing sets
  `LUA_INIT` in the profile.
