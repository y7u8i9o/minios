#!/usr/bin/env python3
"""Build loader rejection fixtures by changing real linked ELF images.

Every executable retains the same code and changes only its DT_NEEDED name.
Libraries are independent files, so guest tests never overwrite a library
that another process might have mapped. The manifest records exit status
and a required diagnostic substring; a signal or timeout is a failure.
"""
import struct
import sys
from pathlib import Path

PH_FIELDS = {"type": (0, "I"), "flags": (4, "I"), "offset": (8, "Q"),
             "vaddr": (16, "Q"), "filesz": (32, "Q"), "memsz": (40, "Q"),
             "align": (48, "Q")}


class Elf:
    def __init__(self, data):
        self.data = bytearray(data)
        self.phoff = self.get(32, "Q")
        self.phnum = self.get(56, "H")
        self.loads = [i for i in range(self.phnum) if self.ph(i, "type") == 1]
        self.dynamic = next(i for i in range(self.phnum) if self.ph(i, "type") == 2)
        start = self.ph(self.dynamic, "offset")
        size = self.ph(self.dynamic, "filesz")
        self.tags = {}
        for off in range(start, start + size, 16):
            tag, value = struct.unpack_from("<qQ", self.data, off)
            if tag == 0:
                break
            self.tags[tag] = (off, value)

    def get(self, offset, fmt):
        return struct.unpack_from("<" + fmt, self.data, offset)[0]

    def put(self, offset, fmt, value):
        struct.pack_into("<" + fmt, self.data, offset, value)

    def ph(self, index, field):
        offset, fmt = PH_FIELDS[field]
        return self.get(self.phoff + index * 56 + offset, fmt)

    def set_ph(self, index, field, value):
        offset, fmt = PH_FIELDS[field]
        self.put(self.phoff + index * 56 + offset, fmt, value)

    def file_offset(self, address):
        for i in self.loads:
            start = self.ph(i, "vaddr")
            if start <= address < start + self.ph(i, "filesz"):
                return self.ph(i, "offset") + address - start
        raise ValueError(f"address {address:x} has no file bytes")

    def table(self, tag):
        return self.file_offset(self.tags[tag][1])

    def set_tag(self, tag, value):
        self.put(self.tags[tag][0] + 8, "Q", value)

    def hide_tag(self, tag):
        # DT_DEBUG is ignored by the loader and leaves the table terminated.
        self.put(self.tags[tag][0], "q", 21)

    def symbol(self, name):
        strings, symbols = self.table(5), self.table(6)
        count = self.get(self.table(4) + 4, "I")
        for i in range(count):
            off = symbols + i * 24
            start = strings + self.get(off, "I")
            end = self.data.index(0, start)
            if self.data[start:end].decode() == name:
                return off
        raise ValueError(name)


cases = []


def case(name, status=127, diagnostic="ld.so:"):
    def register(fn):
        cases.append((name, status, diagnostic, fn))
        return fn
    return register


@case("valid_dual_hash_and_copy", 0, "-")
def valid(e):
    pass


@case("wrong_class", diagnostic="invalid ELF64 shared object")
def wrong_class(e):
    e.data[4] = 1


@case("wrong_endian")
def wrong_endian(e):
    e.data[5] = 2


# The machine numbers of the two architectures, and the GLOB_DAT
# relocation type of each, which makes the loader look up the symbol.
EM_X86_64, EM_AARCH64 = 62, 183
GLOB_DAT = {EM_X86_64: 6, EM_AARCH64: 1025}


@case("wrong_machine")
def wrong_machine(e):
    machine = e.get(18, "H")
    e.put(18, "H", EM_AARCH64 if machine == EM_X86_64 else EM_X86_64)


@case("wrong_phentsize")
def wrong_phentsize(e):
    e.put(54, "H", 55)


@case("wrapped_phoff")
def wrapped_phoff(e):
    e.put(32, "Q", (1 << 64) - 16)


@case("filesz_exceeds_memsz", diagnostic="invalid load segment")
def oversized_file(e):
    i = e.loads[0]
    e.set_ph(i, "filesz", e.ph(i, "memsz") + 1)


@case("file_range_past_eof")
def outside_file(e):
    e.set_ph(e.loads[-1], "offset", len(e.data) + 4096)


@case("wrapped_vaddr")
def wrapped_address(e):
    e.set_ph(e.loads[0], "vaddr", (1 << 64) - 4096)


@case("overlapping_segments", diagnostic="overlapping load segments")
def overlap(e):
    e.set_ph(e.loads[1], "vaddr", e.ph(e.loads[1], "offset") % 4096)


@case("tls_image_larger_than_block", diagnostic="invalid TLS segment")
def tls_sizes(e):
    index = next(i for i in range(e.phnum) if e.ph(i, "type") == 0x6474e551)
    e.set_ph(index, "type", 7)
    e.set_ph(index, "filesz", 8)


@case("tls_image_outside_segments", diagnostic="ELF range outside load segments")
def tls_image(e):
    index = next(i for i in range(e.phnum) if e.ph(i, "type") == 0x6474e551)
    e.set_ph(index, "type", 7)
    e.set_ph(index, "vaddr", 0xfffff000)
    e.set_ph(index, "filesz", 16)
    e.set_ph(index, "memsz", 16)


@case("unterminated_dynamic", diagnostic="unterminated dynamic segment")
def unterminated(e):
    start = e.ph(e.dynamic, "offset")
    for off in range(start, start + e.ph(e.dynamic, "filesz"), 16):
        e.put(off, "q", 21)


@case("bad_string_table")
def string_table(e):
    e.set_tag(5, (1 << 64) - 1)


@case("bad_string_size")
def string_size(e):
    e.set_tag(10, 1 << 63)


@case("zero_sysv_buckets", diagnostic="empty SysV hash table")
def empty_sysv(e):
    e.put(e.table(4), "I", 0)


@case("bad_sysv_index", diagnostic="invalid SysV hash index")
def sysv_index(e):
    e.put(e.table(4) + 8, "I", 0xffffffff)


@case("bad_gnu_bloom", diagnostic="invalid GNU hash header")
def gnu_bloom(e):
    e.put(e.table(0x6ffffef5) + 8, "I", 0)


@case("bad_relocation_symbol", diagnostic="relocation symbol index outside table")
def relocation_symbol(e):
    off = e.table(7)
    e.put(off + 8, "Q", (0xffffffff << 32) | GLOB_DAT[e.get(18, "H")])


@case("relocation_into_code", diagnostic="ELF range outside load segments")
def relocation_target(e):
    text = next(i for i in e.loads if e.ph(i, "flags") & 1)
    e.put(e.table(7), "Q", e.ph(text, "vaddr"))


@case("bad_relocation_size", diagnostic="invalid relocation entry size or format")
def relocation_size(e):
    e.set_tag(9, 16)


@case("unsupported_relocation", diagnostic="unsupported relocation type")
def relocation_type(e):
    off = e.table(7) + 8
    e.put(off, "Q", (e.get(off, "Q") & ~0xffffffff) | 37)


@case("gnu_only", 0, "-")
def gnu_only(e):
    e.hide_tag(4)


@case("sysv_only", 0, "-")
def sysv_only(e):
    e.hide_tag(0x6ffffef5)


@case("unaligned_pure_bss", 0, "-")
def pure_bss(e):
    # Replace the stack note with an extra BSS-only LOAD whose first page
    # has no file backing. Redirect the exported array into that segment.
    index = next(i for i in range(e.phnum) if e.ph(i, "type") == 0x6474e551)
    high = max(e.ph(i, "vaddr") + e.ph(i, "memsz") for i in e.loads)
    start = (high + 4095) // 4096 * 4096 + 0x123
    for field, value in {"type": 1, "flags": 6, "offset": 0x123,
                         "vaddr": start, "filesz": 0, "memsz": 8192, "align": 1}.items():
        e.set_ph(index, field, value)
    e.put(e.symbol("ld_fixture_bss") + 8, "Q", start)


@case("headers_after_first_page", 0, "-")
def late_headers(e):
    headers = e.data[e.phoff:e.phoff + e.phnum * 56]
    offset = (len(e.data) + 4095) // 4096 * 4096
    e.data.extend(bytes(offset - len(e.data)))
    e.data.extend(headers)
    e.put(32, "Q", offset)


@case("invalid_initializer", diagnostic="initializer outside executable segments")
def bad_initializer(e):
    # The array is relocated before it is called. Redirect the corresponding
    # RELATIVE relocation to a readable but non-executable address.
    array = e.tags[25][1]
    rel = e.table(7)
    for off in range(rel, rel + e.tags[8][1], 24):
        if e.get(off, "Q") == array:
            e.put(off + 16, "q", e.get(e.symbol("ld_fixture_data") + 8, "Q"))
            return
    raise ValueError("initializer array has no relocation")


@case("bad_symbol_name", diagnostic="invalid dynamic string")
def symbol_name(e):
    e.put(e.symbol("ld_fixture_value"), "I", e.tags[10][1])


@case("misaligned_load", diagnostic="invalid load segment")
def misaligned(e):
    e.set_ph(e.loads[0], "offset", 1)


@case("missing_library", diagnostic="cannot open library")
def missing(e):
    pass


def main():
    source, program, root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    library_bytes, program_bytes = source.read_bytes(), program.read_bytes()
    libdir, bindir, manifest_dir = root / "lib", root / "bin", root / "usr/share/ldtests"
    for directory in (libdir, bindir, manifest_dir):
        directory.mkdir(parents=True, exist_ok=True)
    manifest = []
    for index, (name, status, diagnostic, mutate) in enumerate(cases):
        image = Elf(library_bytes)
        mutate(image)
        soname = f"libldbad{index:04d}.so"
        if name != "missing_library":
            (libdir / soname).write_bytes(image.data)
        else:
            (libdir / soname).unlink(missing_ok=True)
        executable = bytearray(program_bytes)
        old = b"libldbad0000.so\0"
        assert executable.count(old) >= 1
        executable = executable.replace(old, soname.encode() + b"\0")
        target = bindir / f"ldbad{index:04d}"
        target.write_bytes(executable)
        target.chmod(0o755)
        manifest.append(f"{index}|{status}|{diagnostic}|{name}\n")
    (manifest_dir / "cases").write_text("".join(manifest))
    print(f"loader fixtures: {len(cases)} cases")


if __name__ == "__main__":
    main()
