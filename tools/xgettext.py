#!/usr/bin/env python3
"""Extract the translatable strings of C sources into a .pot file (L3,
docs/design/gettext.md).

    tools/xgettext.py -o user/po/DOMAIN/DOMAIN.pot FILE.c...

A translatable string is the argument of _("..."), N_("...") or
gettext("..."), the first two arguments of ngettext("...", "...", n), or
the context and string of C_("context", "...").  Adjacent string literals
are joined.  The escapes of C are those of the .po format and are copied
unchanged.  Each message lists the files and lines where it occurs."""
import re
import sys

STRING = r'"(?:[^"\\\n]|\\.)*"'
STRINGS = rf'((?:{STRING}\s*)+)'
PATTERNS = [
    ('one', re.compile(r'\b(?:_|N_|gettext)\(\s*' + STRINGS + r'\)')),
    ('plural', re.compile(r'\bngettext\(\s*' + STRINGS + r',\s*' + STRINGS + r',')),
    ('context', re.compile(r'\bC_\(\s*' + STRINGS + r',\s*' + STRINGS + r'\)')),
]


def join(literals):
    """Join adjacent C string literals into the content of one string."""
    return ''.join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', literals))


def po_string(s):
    """Write a .po string, split after each \\n escape."""
    parts = re.split(r'(?<=\\n)', s)
    parts = [p for p in parts if p]
    if len(parts) <= 1:
        return f'"{s}"'
    return '""\n' + '\n'.join(f'"{p}"' for p in parts)


def main():
    args = sys.argv[1:]
    if len(args) < 3 or args[0] != '-o':
        print(__doc__, file=sys.stderr)
        return 2
    out, files = args[1], args[2:]
    messages = {}           # (context, msgid, plural) -> list of references
    for path in files:
        text = open(path, encoding='utf-8').read()
        for kind, pattern in PATTERNS:
            for m in pattern.finditer(text):
                line = text.count('\n', 0, m.start()) + 1
                if kind == 'one':
                    key = (None, join(m.group(1)), None)
                elif kind == 'plural':
                    key = (None, join(m.group(1)), join(m.group(2)))
                else:
                    key = (join(m.group(1)), join(m.group(2)), None)
                if key[1]:
                    messages.setdefault(key, []).append(f'{path}:{line}')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('# Template of the translatable messages, written by tools/xgettext.py.\n')
        f.write('msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n')
        for (ctxt, msgid, plural), refs in sorted(messages.items(), key=lambda kv: kv[1][0]):
            f.write('#: ' + ' '.join(refs) + '\n')
            if ctxt is not None:
                f.write(f'msgctxt {po_string(ctxt)}\n')
            f.write(f'msgid {po_string(msgid)}\n')
            if plural is not None:
                f.write(f'msgid_plural {po_string(plural)}\n')
                f.write('msgstr[0] ""\nmsgstr[1] ""\n\n')
            else:
                f.write('msgstr ""\n\n')
    print(f'xgettext: {len(messages)} messages from {len(files)} files into {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
