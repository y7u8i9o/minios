# English prose rewrite

This plan rewrites the English prose of the entire project: source comments,
documents in `docs/`, manual pages and user-visible messages. The owner found
that nearly all prose reads as word-for-word translation from French or German.
Defects include noun-of-noun chains instead of compound nouns, article overuse,
bare nouns as agentive subjects, literal or archaic phrases, stacked
prepositional phrases, awkward nominalizations, compressed phrasing, padding
with "which means that", and vague adjectives. Each milestone ends with passing
tests and is marked completed here when they pass.

## 1. Scope

The owner chose the following scope on 2026-10-06.

- Rewrite source comments in `.c`, `.h` and `.S` files outside `third_party/`
  and `lib/libwire/generated/`. Measured count: about 1,115 files with roughly
  22,000 comment lines (includes both `//` and `/* */` styles; exact count
  varies).
- Rewrite documents in `docs/`. Measured count: about 26,900 lines.
- Rewrite manual pages in `user/share/man/`. Measured count: about 2,850 lines.
- Rewrite user-visible log and error messages in source files. Messages appear
  as string literals passed to `klog`, `panic`, `printf`, `fprintf`, `perror`
  and similar functions.
- Exclude generated files (`lib/libwire/generated/`), third-party code
  (`third_party/`), and data files (locale catalogues, test expect files remain
  in their current form; a changed message updates its `expect` line in the
  same milestone).

## 2. Fixed decisions

### Writing defects to eliminate

The prose exhibits nine recurring defects:

1. **Noun-of-noun chains** instead of compound nouns or possessives: "the
   authentication dialog of the desktop" (use "the desktop authentication
   dialog"), "the run of sudo" (use "the sudo run"), "the state of the run of
   sudo" (use "the sudo run state"), "the credentials of the user" (use "the
   user credentials" or "the user's credentials"), "the area of the panel" (use
   "the panel area"), "State files of runs of sudo that have ended" (use "State
   files for finished sudo runs").

2. **Definite article overuse**: "the widgets" as a section header (use
   "Widgets"), "the avatar, the full name and the account name" (use "the
   avatar, full name and account name" or "an avatar, a full name and an
   account name").

3. **Bare nouns as agentive subjects**: "Cancel writes nothing" (use
   "Cancelling writes nothing" or "If the user cancels, nothing is written").

4. **Literal or archaic phrases**: "in the manner of GNOME" (use "like GNOME"
   or "in the style of GNOME").

5. **Stacked prepositional phrases**: "sudo runs askpass for 'sudo -A', as
   /etc/sudo.conf names it, with the prompt as its argument and the credentials
   of the user" (break into separate sentences: "sudo runs askpass for 'sudo
   -A', as /etc/sudo.conf specifies. askpass receives the prompt as its
   argument and returns the user credentials").

6. **Awkward nominalizations**: "Its rectangle is the opaque region" (use "Its
   opaque region is its rectangle"), "The size of the screen at the top left
   corner" (use "The screen size from the top left corner").

7. **Compressed phrasing** that hides the consequence: "Gives no reason" (use
   "Gives no reason, so the error is generic"), "Or an empty one" with a
   distant referent (repeat the noun or use a tighter structure).

8. **Padding with "which means that"**: Instead of "indicating that the entered
   password was rejected, which means that authentication failed" use
   "indicating that the entered password was rejected".

9. **Vague adjectives** without the dimension: "so that they are equal" (use
   "for equal width" or "to match heights").

### Constraints preserved from CLAUDE.md

Every rewritten text must continue to follow:

- Professional register as an experienced engineer writes to a client or senior
  colleague.
- Literal meaning only. No metaphors or idioms ("plumbing", "pitfall",
  "shortcut", "under the hood", "out of the box", "the heavy lifting", "where
  it stands", "on track").
- Never any form of "hold", "keep" or "stay" (holds, held, keeps, kept, stays,
  stayed). External names such as GNU ld `KEEP` and the sudo option `env_keep`
  remain.
- Never "name" as a verb (names, named, naming). Use literal facts instead: "the
  table has an entry for the type" or "a branch called develop-x". The noun
  "name" remains allowed.
- No wh-words (what, which, who, where, when, why, how) in headers or bold
  lead-ins. Use noun phrases: "Cases the change needed" instead of "What needed
  testing".
- Comments and documents use short sentences. Each sentence states one fact in
  subject, verb, object order.

### Revised Writing section for CLAUDE.md

The plan proposes this replacement for the Writing section of CLAUDE.md:

```markdown
## Writing

- Write in a professional register, as an experienced engineer writes to a
  client or senior colleague. Sentences are complete, polite and precise, and
  they answer the question asked. Avoid bureaucratic wording. Avoid casual
  wording such as slang, filler, jokes, exclamations, "Sure thing" or "Got it".
- Use words in their literal meaning. Do not use metaphors or idioms, for
  example "plumbing", "pitfall", "shortcut", "under the hood", "out of the box",
  "the heavy lifting", "where it stands" or "on track". State the literal fact
  instead.
- After criticism of a reply or of a piece of work, apologize sincerely first
  and list each mistake. Do not replace the apology with "Fair point" or "You're
  right". Then correct the mistake without justifying it.
- Never use any form of the words hold, keep and stay (holds, held, keeps, kept,
  stays, stayed and so on). The rule covers replies, documents, code comments,
  commit messages and test labels. Names defined by external software, such as
  the GNU ld directive `KEEP` and the sudo option `env_keep`, are the only
  exceptions.
- Never use "name" as a verb (names, named, naming), in any sense. Write the
  literal fact instead, for example "the table has an entry for the type" or "a
  branch called develop-x". The noun "name" is allowed.
- Headers contain no wh words (what, which, who, where, when, why, how). The
  rule covers headings and bold lead-ins. Use noun phrases, for example "Cases
  the change needed" instead of "What needed testing". The rule does not cover
  body text.
- Comments and documents use short sentences. Each sentence states one fact in
  subject, verb, object order. Write the noun for the thing instead of "one",
  "it" or "the latter". Do not chain clauses with ", which". State a cause and
  its result in separate sentences.
- Use compound nouns and possessives instead of chains of "the X of the Y". For
  example: "the desktop authentication dialog" rather than "the authentication
  dialog of the desktop"; "the user credentials" or "the user's password" rather
  than "the credentials of the user". Break long chains into separate sentences.
- Use articles naturally. Omit "the" from headers and when listing properties.
  Use "a" for an instance and "the" for a specific instance already mentioned.
  Do not repeat "the" before each item in a list when one article covers all
  items.
- Use gerunds or explicit subjects for actions. Avoid bare nouns as agents: "If
  the user cancels, nothing is written" or "Cancelling writes nothing" rather
  than "Cancel writes nothing".
- Avoid stacking prepositional phrases. Break complex sentences with many "of",
  "with", "as" or "for" phrases into separate sentences.
- Prefer direct wording over nominalizations. "The function returns the opaque
  region as its rectangle" rather than "Its rectangle is the opaque region".
- State consequences explicitly. Instead of "Gives no reason" write "Gives no
  reason, so the error is generic".
- Remove "which means that" padding. Let the implication be clear from context
  or state it as a separate sentence.
- Specify the dimension with comparisons. "For equal width" rather than "so that
  they are equal".
```

### Enforcement

- `tools/check-prose.py` detects mechanical patterns in comments, documents,
  manual pages and message string literals. It reports findings to stderr with
  file, line number and pattern matched. The script can detect:
  - Forbidden words: any form of hold/keep/stay (except `KEEP`, `env_keep`),
    "name" as verb (named, naming, names followed by object marker).
  - Wh-words in headers (lines starting with `#`, `##`, etc. or bold `**X**:`).
  - Noun-of-noun chains (three or more "of the" or "of a" in one sentence).
  - Stacked prepositions (four or more "of"/"with"/"as"/"for" in one sentence).
  - "which means that" phrase.
  - Bare nouns as subjects before certain verbs (Cancel/Delete/Remove followed
    by verb without -ing form).
  - It cannot reliably detect: article overuse, vague adjectives, awkward
    nominalizations, compressed phrasing. These require human review.
- `tools/prose-exceptions.txt` lists allowed exceptions (file, line, pattern)
  one per line. The owner edits this file for legitimate cases.
- `.claude/settings.json` adds a PostToolUse hook on Edit and Write that runs
  `tools/check-prose.py <changed-file>` and returns exit status 2 if findings
  exist. This blocks the tool result until the author fixes or excepts the
  finding.
- `.git/hooks/pre-commit` runs `tools/check-prose.py` on all staged files and
  rejects the commit if findings exist. The hook is added to the repository.
- `tests/map` adds `host-check-prose = tools/check-prose.py` so that `make
  test-changed` runs the check.

### Verification of prose-only changes

For milestones that rewrite source files, verify that only comments and message
strings changed:

1. Strip comments and extract non-string code:
   `tools/strip-comments.py <file>` removes `//` and `/* */` comments.
   `tools/extract-code.py <file>` removes string literals.
2. Compare stripped files against git HEAD:
   `diff <(tools/strip-comments.py <file>) <(git show HEAD:<file> | tools/strip-comments.py -)`
   The diff must be empty except for changed string literals (messages).
3. For message milestones, verify that only string literals and corresponding
   `expect` files changed:
   `git diff HEAD <file>` shows only modified strings and test expect updates.

The verification runs as part of each milestone's completion check.

## 3. Milestones

### W1. The checker script, the hooks and the revised Writing rules

`tools/check-prose.py` implements pattern detection for the mechanical defects.
It reads `tools/prose-exceptions.txt` for allowed exceptions. The PostToolUse
hook, the pre-commit hook and the host check rule are added. The Writing section
of CLAUDE.md receives the revised rules. The tools are tested against existing
files to calibrate the exception list.

Tests: `make check-prose` runs the checker on a test file with known good and
bad patterns and requires the correct findings. No source files are rewritten
yet, so the exception list initially contains many entries.

Document: `docs/design/build.md` (the checker and the hook).

### W2. Kernel comments

Rewrite comments in `kernel/`. The work divides into batches of about 40 files
each (about 320 files total). Each batch is handed to a Claude Sonnet 4.5
session (`claude -p --model claude-sonnet-4-5`) with a brief containing:

- The revised Writing rules from CLAUDE.md.
- The list of nine defects with examples.
- The rule that meaning and technical facts must be preserved.
- The rule that no comment is deleted except a duplicate.
- The files of the batch.

The session rewrites the comments of each file and reports completed files. The
owner reviews the diffs, rejects files with lost meaning or new errors, and
marks accepted files. A rejected file returns to the next batch with notes.

Tests: After all batches, `make test-changed` runs the affected boot tests.
`tools/strip-comments.py` verifies that code is unchanged. Manual review samples
10% of changed files for defect elimination and preserved meaning.

Document: Already covered in `docs/design/` per subsystem.

Size: About 8,000 comment lines in kernel/, roughly 8 Sonnet 4.5 sessions of 40
files each.

### W3. libc, libgui and other library comments

Rewrite comments in `lib/libc/`, `lib/libgui/`, `lib/libcodec/`, `lib/libaudio/`,
`lib/libm/`, `lib/libwire/` (excluding `generated/`) and other libraries. The
work divides into batches of about 30 files each (about 280 files total). The
batching and review process matches W2.

Tests: After all batches, `make test-changed` runs the affected tests. `make
check-libc`, `make check-libgui`, `make check-libcodec` run the host unit tests.
Verification as in W2.

Document: Already covered in `docs/design/`.

Size: About 5,500 comment lines in libraries, roughly 7 Sonnet 4.5 sessions.

### W4. User program comments

Rewrite comments in `user/`. The work divides into batches of about 35 files
each (about 350 files total). The batching and review process matches W2.

Tests: After all batches, `make test-changed` runs the affected boot tests.
Verification as in W2.

Document: Already covered in `docs/design/`.

Size: About 4,500 comment lines in user/, roughly 6 Sonnet 4.5 sessions.

### W5. Tools and test comments

Rewrite comments in `tools/` and `tests/`. The work divides into batches of
about 30 files each (about 170 files total). The batching and review process
matches W2.

Tests: After all batches, `make test-changed` runs any affected tests.
Verification as in W2.

Document: `docs/design/build.md` (tools).

Size: About 2,500 comment lines, roughly 4 Sonnet 4.5 sessions.

### W6. Documents in docs/

Rewrite all prose in `docs/`. The work divides into batches of about 5 documents
each (about 90 files total in `docs/design/` and `docs/plan/`). Each batch is
handed to a Sonnet 4.5 session with the same brief as W2. The session rewrites
the prose of each document, preserving all technical facts, structure, code
examples and tables. Review as in W2.

Tests: `make check-docs` (if it exists) or manual review of 15% of changed
documents for defect elimination and accuracy. Verification: `git diff` shows
only prose changes, all section headers, code blocks and tables remain.

Document: The documents describe themselves.

Size: About 26,900 lines in docs/, roughly 10 Sonnet 4.5 sessions.

### W7. Manual pages

Rewrite all manual pages in `user/share/man/`. The work treats all ~60 pages as
one or two batches. A Sonnet 4.5 session receives the pages with the same brief
as W2. Manual pages use troff macros; the rewrite changes only the text
arguments of `.TH`, `.SH`, `.TP`, `.PP` and similar, never the macro names or
formatting commands.

Tests: `make check-man` (if added) or manual build and inspection of all pages
with `man`. Verification: diff shows only text changes within macro arguments.

Document: `docs/design/manual.md` (if it does not exist, add a short description
of the manual page format and conventions).

Size: About 2,850 lines, roughly 2 Sonnet 4.5 sessions.

### W8. User-visible messages

Rewrite log and error messages in source files. Messages appear as string
literals passed to `klog`, `panic`, `printf`, `fprintf`, `perror` and similar.
The work divides into batches matching the file groups of W2 to W5. A Sonnet 4.5
session receives the files and rewrites the message strings, preserving format
specifiers and argument order. Boot tests in `tests/cases/*/expect` match
messages by regular expression; a changed message requires the corresponding
`expect` line updated in the same commit.

Tests: After each batch, `make test-changed` runs affected boot tests and
verifies that expect files match new messages. Verification: `git diff` shows
only string literal changes and corresponding expect updates.

Document: `docs/design/testing.md` (the expect files).

Size: About 3,500 message strings across all areas, roughly 6 Sonnet 4.5
sessions.

### W9. Final cleanup and exception list reduction

Review `tools/prose-exceptions.txt` and eliminate entries that are no longer
needed after the rewrites. Run `tools/check-prose.py` on the entire repository
and verify that all remaining findings are legitimate exceptions or patterns the
checker should not detect. Update the checker if needed. Run the full test suite
(`make test CASES="..."`; select a representative subset of about 30 cases
covering all major subsystems) to verify that no regressions were introduced.

Tests: `make check-prose` reports zero findings outside the exception list. A
subset of boot tests passes. Manual review confirms the exception list contains
only legitimate cases.

Document: `docs/design/build.md` (final exception list and checker status).

Size: Review and cleanup, 1 session.

## 4. Size

The work changes approximately:

- 22,000 comment lines in about 1,115 source files.
- 26,900 lines in about 90 documents.
- 2,850 lines in about 60 manual pages.
- 3,500 message strings and corresponding expect lines.

Total: about 55,000 lines of prose. The work divides into roughly 44 Claude
Sonnet 4.5 sessions of 30 to 40 files or 5 documents each, plus tooling and
review. Each session processes one batch, the owner reviews and accepts or
rejects each file, and rejected files return to the next batch with feedback.
