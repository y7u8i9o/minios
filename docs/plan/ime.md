# Input methods

This plan replaces the input methods of L6 (`locale.md`) with an input
method daemon modelled on IBus, a pinyin engine with phrases and sentences
from a Rime dictionary, and a Japanese engine with kana to kanji conversion
from the Mozc dictionary. Milestone identifiers use the prefix `I`. Each
milestone ends with a boot test, updates `docs/design/ime.md`, and is marked
completed here when its boot tests pass. The work is on the branch
`bleeding-edge-ime`.

## 1. Motivation and scope

The L6 engines in the compositor convert one character at a time from
Unihan tables. A Chinese user types words and sentences, and a Japanese user
converts a whole reading into segments of kanji and kana. Super+Space
selects the L6 engines. On a macOS host Cmd+Space opens Spotlight and
Ctrl+Space switches the input source of macOS, so neither reaches QEMU.

## 2. Fixed decisions

- The roles of IBus map to minios as follows. The input context is the
  `text_input` context of each client. The compositor relays between the
  contexts and the daemon, as ibus-daemon does on its bus. The daemon `imed`
  handles the switch keys, contains the engines and draws the candidate
  window. The panel indicator and its menu show and select the engine.
  Each key goes to `imed`, which replies whether it used the key, as the
  ProcessKeyEvent call of IBus does.
- A Shift tap (a press and a release of Shift without another key) toggles
  between the engine and direct input, as in Rime, Sogou and Microsoft
  Pinyin. Ctrl+Shift cycles the layout and the engines, as in Windows.
  Ctrl+Space and Super+Space toggle between the layout and the last engine,
  as in IBus and fcitx, for hosts where they reach the guest.
- The pinyin dictionary is `pinyin_simp.dict.yaml` of rime-pinyin-simp
  (Apache-2.0): 17000 characters and 48000 words with weights, in
  simplified Chinese.
- The Japanese dictionary is `dictionary_oss` of Mozc (IPAdic license and
  BSD 3-clause): readings, parts of speech, costs and the connection costs
  between parts of speech.
- The source data is downloaded by `tools/fetch_imedata.sh` into
  `third_party/imedata`, which is not in the repository. `tools/genime.py`
  writes the binary dictionaries into `user/share/ime`, which are checked
  in with the license notices.
- Without `imed` the compositor composes with its built-in Ctrl+Shift+U
  entry and the dead keys of the layout, as ibus-engine-simple does.

## 3. Milestones

### I0. Switch keys that work on macOS (completed 2026-10-03)

- A Shift tap toggles the L6 engine and the layout, Ctrl+Shift cycles the
  layout, Japanese and Chinese, and Ctrl+Space and Super+Space toggle
  between the layout and the last engine. A click on the panel indicator
  cycles.
- The `ime` case uses Ctrl+Shift and a Shift tap.

Ctrl+Shift acts on its tap, as in Windows, which leaves Ctrl+Shift+U and
the other Ctrl+Shift shortcuts working. The panel selects the next method
through the new settings key `input_method`. The cases `ime`, `comp_seat`,
`comp_panel`, `keymap`, `gui_editor`, `gui_region` and `gui_settings`
pass.

### I1. Protocol and the daemon (completed 2026-10-03)

- `protocol/ime.xml` with `input_method_manager`, `input_method`,
  `candidate_surface` and `ime_control`. The compositor sends the keys of
  an active context to `imed` and waits at most 150 ms for the reply. The
  candidate surface is placed below the caret of the active context.
- `imed` with an engine interface and the switch keys of I0, started by
  `startgui`.
- The boot test is `ime_protocol`, with a test input method.

The test input method is the test engine of `imed -t` instead of a
separate program. The compositor sends the keys of an active context only,
and the switch keys stay in the compositor, as GNOME Shell does with IBus.
`startgui` starts `imed` from I3 on, when the daemon has its first engine.
The cases `ime_protocol`, `ime`, `comp_seat`, `comp_core`, `comp_shell`,
`gui_editor` and `keymap` pass.

### I2. Candidate window (completed 2026-10-03)

- A lookup table as in IBus: page size, cursor, labels, horizontal or
  vertical layout, an auxiliary line, page arrows, mouse selection.
- The boot test is `ime_candidates`.

The page size and the orientation come from `ime_page_size` and
`ime_orientation` of `desktop.conf`, which I5 adds to Settings. The cases
`ime_candidates`, `ime_protocol` and `ime` pass.

### I3. Pinyin engine

- Segmentation into syllables with ambiguity, incomplete syllables and
  abbreviations by initials. Candidates of a sentence, of words and of
  characters. Partial selection, paging, Chinese punctuation, a Shift tap
  for English, and a user dictionary that learns chosen words and phrases.
- The L6 engines, their Unihan tables and `tools/fetch_unihan.sh` are
  removed.
- Host unit tests, and the boot test `ime_pinyin`.

### I4. Japanese engine

- Romaji to kana, conversion of a reading into segments by the costs of the
  Mozc dictionary, segment movement and resizing, candidates of a segment,
  F6 to F10, the Japanese keys of JIS keyboards and the Kana and Eisu keys
  of Mac keyboards, and a user history.
- Host unit tests, and the boot test `ime_japanese`.

### I5. Settings, panel menu and terminal

- An input method section on the Region and language page, a menu on the
  panel indicator, and text input in the terminal.
- The boot test is `gui_ime_settings`.
