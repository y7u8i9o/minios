# X12 display protocol and libwire

M24 introduces the protocol between X12 and its clients and
the library both sides use. It is Wayland inspired but its own: object
ids, opcodes and a binary wire format over a Unix domain socket with
descriptor passing, interfaces described in XML, and a scanner that
generates the C marshalling code. No `wl_` or `xdg_` names are used.

## Wire format

- A message is a header of 8 bytes, the object id (u32), the opcode
  (u16) and the total size in bytes (u16), followed by the arguments.
- Arguments: `int` and `uint` in 4 bytes, `fixed` as 24.8 in 4 bytes,
  `string` as a u32 length including the terminating NUL then the bytes
  padded to 4, `array` as a u32 length then bytes padded to 4, `object`
  and `new_id` as u32 ids (0 is null where the signature allows it),
  `fd` out of band in an `SCM_RIGHTS` record, in argument order across
  the messages of one flush. A signature string encodes the types
  (`i u f s a o n h`, with `?` before a nullable argument).
- Clients allocate ids from 1 upwards and reuse an id after the
  server's `display.delete_id`; the server allocates from `0xff000000`.
- X12 listens on the abstract socket name `display`
  (`user/compositor/main.c`); clients connect through
  `wire_display_connect`.

## Protocol definition and scanner

`protocol/core.xml` describes the core interfaces: `display` (sync,
get_registry; error, delete_id), `registry` (bind; global,
global_remove), `callback` (done), `compositor` (create_surface),
`shm` (create_pool; format), `shm_pool` (create_buffer, resize,
destroy), `buffer` (destroy; release), `surface` (attach, damage,
frame, set_opaque_region, set_input_region, set_buffer_scale,
set_buffer_transform, damage_buffer, commit, destroy; enter) and
`output` (geometry, mode, scale, transform, done). The `shell` global
also carries the liveness pair `ping` (event) and `pong` (request) and
`set_pid`, used for the not responding dialog (`compositor.md`). The shell protocol
adds toplevel parent/modal state and popup configure/acknowledge/grab;
`protocol/text.xml` has the UTF-8 text input and preedit events, and since
version 2 the caret rectangle for the candidates of an input method. The
seat of version 2 reports the label of the input method (`ime.md`).
`protocol/data.xml` has the selection and drag and drop; its version 2
adds the copy and move actions (`dnd.md`).
`protocol/ime.xml` connects the compositor and the input method daemon
`imed` (`ime.md`).
`protocol/debug.xml` contains the `debug`, `tracer`, `settings` and
`screencopy` interfaces of the debugging tools and of `screenshot`
(`tools.md`, `images.md`). The `debug` interface of version 2 adds the
frame statistics (`graphics-performance.md`). `gui_bind_global` treats a
global whose advertised version is lower than the requested one as
missing, because a bind above the advertised version ends the connection.
Requests marked `type="destructor"` destroy their object; the server
removes the resource after the handler ran.

`tools/wscan/wscan.py` reads the XML and writes into
`lib/libwire/generated/`: `core-client.h/.c` (one function per request,
returning the new proxy for `new_id` arguments; a `struct X_listener`
per interface with one callback per event and `X_add_listener`),
`core-server.h/.c` (a `struct X_impl` per interface with one handler
per request; one `X_send_event` function per event) and
`core-interfaces.c` (the shared descriptors: names, signatures and
argument interfaces). The generated files are committed; `make -C
libwire generate` regenerates them.

## libwire (`lib/libwire/`)

- `wire/common.h`: `struct wire_conn` with the output buffer and queued
  descriptors, the input buffer and received descriptors;
  `wire_conn_marshal` appends a message (flushing first when buffers
  would overflow or more than 16 descriptors are queued),
  `wire_conn_flush` sends with `sendmsg`, `wire_conn_read` receives
  with `recvmsg` and queues descriptors, `wire_conn_peek` frames the
  next complete message and `wire_unmarshal` decodes it against a
  signature, taking descriptors from the queue in order.
- `wire/client.h`: `struct wire_display` (connection, proxy table, id
  allocator, error state) and `struct wire_proxy` (id, interface,
  listener, user data). `wire_display_dispatch` reads once and delivers
  events to listeners through a generic trampoline (arguments are
  passed as machine words, object arguments resolved to proxies);
  `wire_display_roundtrip` performs `display.sync` and dispatches until
  its callback fires; protocol errors set `wire_display_error`.
- `wire/server.h`: `struct wire_server` (listening socket, clients,
  globals, serial counter), `struct wire_client` (connection,
  resources, user data with a destroy hook) and `struct wire_resource`
  (id, interface, handlers, data, destroy hook). The registry is
  implemented by the library: `wire_global_create` advertises a global
  to every registry and calls the global's bind function on
  `registry.bind`; `display.sync` is answered with `callback.done`
  immediately. Requests on unknown objects, unknown opcodes, malformed
  bodies or objects of the wrong interface post `display.error` and
  disconnect the client.
- Tracing: `wire_server_set_trace` installs a hook that the server calls
  for every request after it is decoded and before its handler runs,
  and for every event as it is queued, with the client, the object, the
  opcode, the decoded arguments (object and new id arguments as ids)
  and the encoded size. The hook is one pointer test when none is
  installed. `wire_format_args` in `wire/common.h` writes the arguments
  of a message as text: integers, fixed point numbers with two
  decimals, quoted strings with escapes cut after 60 bytes, `nil`,
  `interface@id` for objects when a resolver names them, `new
  interface@id` for new ids, `array[size]` and `fd N`. An untyped new
  id takes its interface from the last string argument before it,
  which makes `registry.bind` read `new seat@5`. X12 uses both for the
  `tracer` interface (`compositor.md`, `tools.md`).

## Tests

`make check` runs `lib/libwire/tests/test_wire.c` on the host over a
socketpair with both sides in one process: registry and bind, every
argument type including a null object and an array, descriptor
passing with `create_pool` (a temporary file read on the server side),
events with arguments, destructor requests and id reuse after
`delete_id`, `sync`, and a protocol error. It also installs a trace
hook and checks the traced lines of requests and events, their sizes,
that removing the hook stops the trace, and the formatter's fixed
point numbers, escaped and cut strings, descriptors and untyped new
ids. `comp_core` covers the same
protocol in the target against the compositor.

Messages carry at most 16 arguments (`WIRE_MAX_ARGS`). The listener and handler trampolines in `lib/libwire/src/client.c` and `server.c` cover 0 to 12 arguments, so a message with more arguments must be split.

## Window geometry

`toplevel.set_window_geometry(x, y, width, height)` (added with the
client side decorations) tells the server which part of the surface is
the visible window: the header bar and contents without the shadow
margins around them. Configure sizes, placement, clamping and the
maximized size refer to that rectangle; a buffer committed after an
acknowledged configure must carry a geometry of the configured size. A
width or height of 0 clears it and the whole surface counts again.
