# Input methods

The compositor X12 has two input methods, one for Japanese and one for
Simplified Chinese. They compose text for the clients that use the text
input protocol, which are the text widgets of libgui. The panel shows the
current choice next to the mixer button. `docs/plan/ime.md` replaces these
engines with an input method daemon.

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
`input_method` with the value -1, and `seat_select_input_method` selects
the next method.

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

## Engines

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
