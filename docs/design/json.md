# JSON output

`libjson` writes JSON. It is a shared library of its own, `libjson.so` in
the package `libjson`, with its header `json/json.h` and the static
archive in `libjson-dev`. The profiler client (`libprof`) exports its
reports with it, and `sysinfo --json` writes the device tree with it. A
package that uses it needs `libjson.so`, and `pkg` installs the provider
from the repository (`packages.md`).

| File | Content |
|---|---|
| `libjson/include/json/json.h` | the interface |
| `libjson/src/json.c` | the escaper and the writer |
| `libjson/Makefile` | `libjson.so`, `libjson.a`, ABI 1 |

## Strings

`json_write_string(FILE *f, const char *s)` writes `s` with its quotes:

- `"` and `\` are written as `\"` and `\\`.
- Control characters below 0x20 and DEL are written as `\uXXXX`.
- A valid UTF-8 sequence (RFC 3629) is copied unchanged.
- A byte that does not begin a valid sequence is written as `\u00XX`, the
  code point of the byte in Latin-1. Symbol names of the profiler are bytes
  in no defined encoding, and such output is valid JSON with this rule.
- `NULL` is written as `""`.

The function was the private `json_string` of the profiler report before.
It moved here unchanged except for the treatment of valid UTF-8, which the
profiler wrote as one `\u00XX` per byte.

## The writer

`struct json_writer` writes one document in order:

```c
struct json_writer w;
json_init(&w, stdout);
json_begin_object(&w);
json_key(&w, "name");
json_string(&w, "vda");
json_key(&w, "sectors");
json_uint(&w, 1048576);
json_end_object(&w);
if (json_finish(&w) < 0)
    ...
```

It places the commas, and it records an error for a call out of order:

- a key outside an object, or two keys without a value between them;
- a value in an object without a key;
- a second value at the top level;
- a close of the wrong container;
- nesting beyond 32 levels.

After an error the writer writes nothing more. `json_finish` adds a
newline and returns -1 after an error, when a container is still open, or
when the stream reported an error.

The values are strings, `json_int`, `json_uint`, `json_bool` and
`json_null`. The writer has no floating point values, because the
programs that use it have none.
