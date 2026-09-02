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
`output` (geometry, mode, scale, transform, done). The shell protocol
adds toplevel parent/modal state and popup configure/acknowledge/grab;
`protocol/text.xml` carries UTF-8 text-input and preedit events.
Requests marked `type="destructor"` destroy their object; the server
removes the resource after the handler ran.

`tools/wscan/wscan.py` reads the XML and writes into
`libwire/generated/`: `core-client.h/.c` (one function per request,
returning the new proxy for `new_id` arguments; a `struct X_listener`
per interface with one callback per event and `X_add_listener`),
`core-server.h/.c` (a `struct X_impl` per interface with one handler
per request; one `X_send_event` function per event) and
`core-interfaces.c` (the shared descriptors: names, signatures and
argument interfaces). The generated files are committed; `make -C
libwire generate` regenerates them.

## libwire (`libwire/`)

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

## Tests

`make check` runs `libwire/tests/test_wire.c` on the host over a
socketpair with both sides in one process: registry and bind, every
argument type including a null object and an array, descriptor
passing with `create_pool` (a temporary file read on the server side),
events with arguments, destructor requests and id reuse after
`delete_id`, `sync`, and a protocol error. `comp_core` covers the same
protocol in the target against the compositor.

Messages carry at most 16 arguments (`WIRE_MAX_ARGS`). The listener and handler trampolines in `libwire/src/client.c` and `server.c` cover 0 to 12 arguments, so a message with more arguments must be split.
