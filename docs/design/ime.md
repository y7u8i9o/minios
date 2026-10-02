# Input methods

Input methods compose text for the clients that use the text input
protocol, which are the text widgets of libgui. The compositor X12 has two
built-in engines from L6, one for Japanese and one for Simplified Chinese,
and relays to the input method daemon `imed`, which `docs/plan/ime.md`
introduces after the model of IBus. The panel shows the current method
next to the mixer button.

## Methods

`user/compositor/inputmethod.c` keeps the list of methods that the switch
keys select. The keyboard layout is the first method. The engines of the
daemon follow in the order of its `set_engines` request, then the built-in
engines `l6-japanese` and `l6-chinese`. The first toggle selects the first
engine whose name contains `pinyin` or `chinese`. `im_select` selects a
method, `im_toggle` toggles between the layout and the last engine, and the
settings key `input_method` of the panel and the `ime_control` interface
select a method as well.

## Switch keys

The switch keys work on a macOS host, where Cmd+Space opens Spotlight and
Ctrl+Space switches the input source of macOS before QEMU sees them.

| Keys | Effect |
|---|---|
| a Shift tap | toggles between the layout and the last engine (the Chinese engine at first) |
| a Ctrl+Shift tap | selects the next method in the order layout, Japanese, Chinese |
| Ctrl+Space, Super+Space | toggle as a Shift tap does, where the host passes them |
| a click on the panel label | selects the next method |

A tap is a press and a release without another key between them. Shift
with a letter therefore types a capital, and Ctrl+Shift+U still starts the
Unicode entry. A change of method commits the composition as it is shown.
`seat.c` detects the taps. The panel sends the settings key
`input_method` with the value -1, and `im_select` selects the next method.

## Input method daemon

`protocol/ime.xml` connects the compositor and `imed` (`user/imed/`). The
roles follow IBus. The text input context of each client is the input
context, the compositor relays as the bus of ibus-daemon does, and the
daemon contains the engines and draws the candidate window.

| Interface | Use |
|---|---|
| `input_method_manager` | the global: `get_input_method` for one client at a time, `get_control` for any client |
| `input_method` | keys and context state to the daemon, text changes and the engine list to the compositor |
| `candidate_surface` | the role of the surface of the candidate window |
| `ime_control` | the methods and the current one, and `select`, for the panel and the settings |

The daemon is active while one of its engines is selected and a text input
context has the keyboard focus. It then receives `activate`, the
surrounding text, the content type and the caret in screen coordinates,
and `deactivate` when the focus or the method changes. Every key of the
active context goes to the daemon as `key` with a serial. The compositor
holds the key in a queue of 32 entries until `key_handled` arrives. A used
key and its release do not reach the client. A passed key goes the usual
way through the dead keys of `text.c` to the client, with the modifiers of
the moment it was typed. A key without a reply in 150 ms passes, and the
compositor logs a timeout. The keys typed after a waiting key wait behind
it, so the order stays. A key with Ctrl, Alt or Super goes to the client at
once and to the daemon with serial 0, and the daemon commits its
composition. The daemon sends the changes of a key before the reply, so the
compositor applies the text before it passes the key.

`commit_string`, `preedit_string` and `delete_surrounding_text` apply
together at `commit`. They reach the focused context while the daemon is
active, and also after a change of method as long as the same surface has
the focus. A Shift tap therefore commits the composition of the engine
when it selects the layout: the compositor sends `select_engine` with the
new method first, and the daemon commits as the engine shows it. When the
focus moves, the daemon drops its composition, and the compositor removes
the preedit from the context that loses the focus.

The candidate surface has the role `ROLE_IME_POPUP`. The compositor places
it 2 pixels below the caret, or above the caret when the screen ends below,
inside the screen, and above every other surface. It is shown only while
the daemon is active and never takes the keyboard focus. The pointer
reaches it, so the daemon can take clicks on candidates.

`imed` uses raw libwire like the panel. Each engine is a `struct
imed_engine` with `select`, `key`, `flush`, `reset` and
`candidate_clicked`. An engine changes the composition through
`imed_commit`, `imed_preedit` and the lookup table `imed_table`, and the
daemon sends the changes after each event. `imed -t` adds the test engine
of the boot tests: letters compose, the candidates are the letters in
capitals, in small letters and with a capital first, and F12 replies after
300 ms.

## Candidate tables

`tools/fetch_unihan.sh` downloads the Unihan database of Unicode 16.0 into
`third_party/unihan`, which is not in the repository. `tools/genime.py`
reads it and writes two tables into `user/share/ime`, which are checked in
and installed as `/usr/share/ime`. Each line of a table is a reading, a tab
and the characters with that reading, without separators. The lines are
sorted by reading in code point order, which is the byte order of UTF-8.

`kana.tab` has 4891 readings in hiragana from the field `kJapanese`. Its
katakana readings (the on readings) are converted to hiragana. The Jōyō
kanji (`kJoyoKanji`) come first. Within each group a kanji that names the
reading earlier in its list of readings comes first, then a kanji with
fewer readings, then the lower code point. The order gives 山 for やま, 川
for かわ and 人 for ひと as the first candidate.

`pinyin.tab` has 393 syllables without tone marks, with ü written as v,
from the field `kHanyuPinlu`. The characters are ordered by the frequency
that the field gives. Traditional characters, which have a
`kSimplifiedVariant` other than themselves, are left out. The order gives
中 for zhong and 国 for guo.

The compositor reads a table when its engine is first selected. It builds
an array of the readings and finds a reading by binary search.

## Built-in engines

`user/compositor/ime.c` contains both engines. The engines do not use the
protocol. `ime_key` receives the key code, the character of the layout and
the modifiers, and returns whether the engine used the key. Its result has
the text to commit and the new preedit, which `text.c` sends to the focused
text input context. A key that an engine used does not reach the client as
a key event, and neither does its release. The Ctrl+Shift+U composer and a
dead key cancelled with Escape or Backspace also consume their keys.

The Japanese engine converts romaji to hiragana while the letters are
typed. A sequence that begins a longer sequence waits, a doubled consonant
gives a small っ, and an n before a consonant gives ん. The punctuation keys
`-`, `,`, `.`, `[` and `]` give ー, 、, 。, 「 and 」.

| Key | Without candidates | With candidates |
|---|---|---|
| Space | the candidates of the longest reading at the start of the text | the next candidate (Shift+Space the previous one) |
| arrows | no effect | the next or the previous candidate |
| 1 to 9 | a digit in the text | the candidate with that number on the page |
| Enter | commits the text | commits the selected candidate |
| Backspace | deletes the last romaji letter or kana | closes the candidates |
| Escape | cancels the text | closes the candidates |
| F7 | switches the text between hiragana and katakana | no effect |

The candidates are the kanji of the reading, then the reading in hiragana
and in katakana. Without a matching reading the candidates are the whole
text in hiragana and in katakana. A chosen candidate replaces its reading,
and the rest of the text stays in the preedit for the next conversion. A
letter typed while the candidates are shown commits the selected one first.

The Chinese engine collects pinyin letters and shows the candidates of the
longest syllable at their start after every letter. Space commits the
selected candidate and 1 to 9 the candidate with that number. The letters
after the syllable then get their own candidates. Typing `zhongguo` and
Space twice gives 中国. An apostrophe separates syllables. Enter commits
the letters, Backspace deletes the last letter and Escape cancels.

While Ctrl, Alt or Super is down, an engine commits its text as it is shown
and passes the key to the client.

## Candidate box

The box shows the page of nine candidates that contains the selected one,
each with its number, and the page number when there is more than one
page. It is drawn with the theme of the decorations after all surfaces in
`scene.c`. It appears below the caret, or above the caret when there is no
room below, and moves with the caret. Every change of the candidates or of
the caret damages the old and the new box.

The caret comes from the request `set_cursor_rectangle` of `text_input`
version 2, in surface coordinates, applied at the next commit. The editor
and the text field of libgui report the start of the preedit through
`widget_text_cursor`, and libgui sends only a changed rectangle. Without a
rectangle the box appears near the top left corner of the surface.

## Label

The seat of version 2 has the event `input_method` with a short label: あ
for the Japanese engine, 拼 for the Chinese engine, and otherwise the
layout name in capitals. The `us` layout and the first group of a layout
with two groups are EN. The compositor sends the label after the bind,
after a change of method, after Alt+Shift changes the group and after a
keymap reload. The panel binds the seat with a listener and draws the label.

## Limits

The engines convert one character at a time. They have no dictionary of
words, no learning and no conversion of a whole phrase. The Chinese engine
does not accept abbreviated pinyin such as `zg` for zhongguo. The engines
compose only in clients with text input enabled. The terminal receives key
events and is not covered.

## Test

`ime` starts the panel and gedit and selects the Japanese engine with a
Ctrl+Shift tap. It types `yama` and chooses 山 with Space and Enter,
chooses 水 for `kawa` with a second Space, chooses 二 for `ni` with the
digit 2, and commits `kana` once as hiragana and once in katakana after
F7. A Ctrl+Shift tap selects the Chinese engine, which composes `zhongguo`
and `nihao` with Space after each syllable. A Shift tap selects the layout
for `a`, another one the Chinese engine for `hao`, another one the layout
for `b`, and Ctrl+Space the Chinese engine for `ni`. gedit must save
山水二かなカナ中国你好a好b你, which also shows that no Space, Enter or digit
reached gedit as a key. A click on the panel label must then select the
layout, and the compositor log must report the labels あ, 拼 and EN.

`ime_protocol` starts the compositor, `imed -t` and gedit, and selects the
test engine with a Ctrl+Shift tap. It composes `abc` and chooses ABC with
Space, `de` with the digit 2, `Fg` with Right twice and Space, sends Enter,
which the engine does not use and gedit receives as a new line, and F12,
which times out and reaches gedit. It composes `hi` and selects the layout
with a Shift tap, which commits hi, and types `x`. gedit must save
`ABCdeFg`, a new line and `hix`. The log must show the bound input method,
the candidate surface and its place, the timeout and the end of the input
method.
