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

## Dictionaries

`tools/fetch_imedata.sh` downloads the dictionaries of the engines of
`imed` at pinned commits into `third_party/imedata`, and
`tools/fetch_unihan.sh` downloads the Unihan database of Unicode 16.0 into
`third_party/unihan`. Neither directory is in the repository.
`tools/genime.py` reads both and writes the files of `user/share/ime`,
which are checked in and installed as `/usr/share/ime`.

`pinyin.dict` comes from `pinyin_simp.dict.yaml` of rime-pinyin-simp
(Apache-2.0, its license is installed beside it as
`pinyin_simp.LICENSE`): 415 syllables and 65125 entries of one to four
syllables with weights, 17000 characters and 48000 words of simplified
Chinese, ü written as v. The file is little endian. A header with the
magic `MPY1` gives the number of syllables and entries and the offsets of
the syllable table, the entry table and the pool. The syllables are in
alphabetical order, 8 bytes each. An entry has 16 bytes: four syllable
numbers (0xffff after the last), the weight and the offset of the word in
the pool of zero-terminated UTF-8 words. The entries are sorted by their
syllable numbers. Because the syllables are sorted, the syllables that
begin with some letters have consecutive numbers, so an incomplete
syllable or an initial stands for a range of numbers. `imed` reads the
file when it starts.

`kana.tab`, for the built-in Japanese engine, has one line per reading: the
reading in hiragana, a tab and its kanji, sorted by reading. It has 4891
readings from the field `kJapanese` of Unihan, with the katakana readings
converted to hiragana. The Jōyō kanji (`kJoyoKanji`) come first. Within
each group a kanji that names the reading earlier in its list of readings
comes first, then a kanji with fewer readings, then the lower code point.
The order gives 山 for やま, 川 for かわ and 人 for ひと as the first
candidate. The compositor reads the table when the engine is first
selected and finds a reading by binary search.

## Candidate window

The candidate window of `imed` (`user/imed/window.c`) shows the lookup
table of the engine as IBus does: the candidates with a label and an
optional comment, the cursor on one of them, a page size, a horizontal or
a vertical layout and an auxiliary line above the candidates, where the
pinyin engine shows the segmented syllables. The window shows the page
that contains the cursor and labels its candidates 1 to the page size.
With more than one page, the arrows ‹ and › at the end turn the pages.
The cursor candidate has the selection colour of the theme. The window is
drawn with the libgui painter in a 16 pixel font, with the CJK fallback
font, at the scale of the output, and the daemon hides it by attaching no
buffer when the table is empty.

The configuration keys `ime_page_size` (2 to 9, 5 by default) and
`ime_orientation` (`horizontal` or `vertical`) of `desktop.conf` apply at
the next key. The digits choose on the current page (`imed_page_first`). A
click on a candidate passes it to `candidate_clicked` of the engine, a
click on an arrow and the wheel turn the pages.

## Pinyin engine

The pinyin engine of `imed` (`user/imed/pinyin.c` with the core
`pycore.c`) follows the keys of Rime. Its label is 拼, and the first
Shift tap selects it.

The core divides the letters into a lattice of syllable edges. An edge is
a whole syllable, an incomplete syllable at the end of the letters (zhon),
or an abbreviation by the initial (z, or zh, ch, sh) where the next letter
cannot continue a syllable: the z of zg, not the z of zhong. An apostrophe
ends a syllable. Each edge stands for a range of syllable numbers. A
search over the paths of edges from each position collects the words of
the dictionary and of the user dictionary whose syllables fall in the
ranges, as word edges. The score of a word is the logarithm of its
probability, its weight divided by the sum of all weights, less 3 for each
abbreviated and 1 for each incomplete syllable. The division of the
letters into the fewest and most complete syllables is the auxiliary line
(`zhong'guo`), where an abbreviation costs 2.5 syllables and a syllable
without a vowel (n, m, ng), an interjection, costs 3. A Viterbi search over
the word edges that follow this division, without abbreviations and
interjections, with 0.5 less per word, gives the sentence.

The candidates are the sentence when it has more than one word, then the
words at the start, the longer ones first and each length by falling
score: for `jintiantianqihenhao` the sentence 今天天气很好, then 今天, then
single characters. Each candidate covers some letters. A candidate that
covers only the first letters is fixed in the preedit, and the rest of the
letters gets new candidates: 2 for `woaibeijing` fixes 我爱 and leaves
`beijing` for 北京.

| Key | Effect |
|---|---|
| a to z, apostrophe | add to the input |
| Space, 1 to 9 | choose the cursor candidate, or the candidate with that number on the page |
| arrows | move the cursor |
| `-` `=` `,` `.`, PageUp, PageDown | turn the pages |
| Enter | commit the fixed words and the letters as they are |
| Escape | drop the input |
| Backspace | delete the last letter, or with no letters left undo the last fixed word |
| other punctuation | commit the input with the first candidate at each step, then the Chinese punctuation |

Without input the punctuation keys give the Chinese punctuation: ，。？！；：
（）【】《》、 for `, . ? ! ; : ( ) [ ] < > \`, …… for `^`, —— for `_`, ～ for
`~`, ￥ for `$`, and quotes “ ” and ‘ ’ that alternate. Digits and the other
keys pass. A Shift tap commits the input as it is and selects the layout.

When the input is used up, the fixed words are committed and learned in
`$HOME/.config/imed/pinyin.user`: one line `word TAB syllables TAB count`
per choice, where a later line replaces the count of an earlier one. A
phrase of several fixed words is learned as a word. A learned word scores
4 plus twice the logarithm of one plus its count more, and a learned
phrase that the dictionary lacks scores as a word of weight 1000 plus that.
After 中国 was chosen once, `zg` offers it first.

The daemon loads the dictionary and the CJK font when it starts, and it
replies to a key before it draws the candidate window, so the first keys
do not wait for the font. It logs a key that took the engine more than
100 ms.

## Built-in engines

`user/compositor/ime.c` contains the Japanese engine of L6, until the
Japanese engine of `imed` replaces it in I4. The Chinese engine of L6 is
replaced by the pinyin engine. The engine does not use the protocol. `ime_key` receives the key code, the character of the layout and
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

While Ctrl, Alt or Super is down, the engine commits its text as it is
shown and passes the key to the client.

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

The seat of version 2 has the event `input_method` with a short label: the
label of the engine (あ for the built-in Japanese engine, 拼 for the pinyin
engine), and otherwise the layout name in capitals. The `us` layout and the first group of a layout
with two groups are EN. The compositor sends the label after the bind,
after a change of method, after Alt+Shift changes the group and after a
keymap reload. The panel binds the seat with a listener and draws the label.

## Limits

The built-in Japanese engine converts one character at a time. The pinyin
dictionary has words of at most four syllables, and longer text comes from
sentences. The pinyin engine has no fuzzy syllables (zh for z, ing for in)
and no caret inside the input. The compositor repeats no key for the
daemon, so a held Backspace deletes one letter of the input. The engines
compose only in clients with text input enabled. The terminal receives key
events and is not covered.

## Test

`ime` starts the panel and gedit and selects the built-in Japanese engine
with a Ctrl+Shift tap. It types `yama` and chooses 山 with Space and Enter,
chooses 水 for `kawa` with a second Space, chooses 二 for `ni` with the
digit 2, and commits `kana` once as hiragana and once in katakana after
F7. A Shift tap selects the layout for `a`, another one the engine for
`yama`, and Ctrl+Space the layout for `b`. gedit must save 山水二かなカナa山b,
which also shows that no Space, Enter or digit reached gedit as a key. A
click on the panel label must then select the engine, and the compositor
log must report the labels あ and EN.

`ime_protocol` starts the compositor, `imed -t` and gedit, and selects the
test engine with a Ctrl+Shift tap. It composes `abc` and chooses ABC with
Space, `de` with the digit 2, `Fg` with Right twice and Space, sends Enter,
which the engine does not use and gedit receives as a new line, and F12,
which times out and reaches gedit. It composes `hi` and selects the layout
with a Shift tap, which commits hi, and types `x`. gedit must save
`ABCdeFg`, a new line and `hix`. The log must show the bound input method,
the candidate surface and its place, the timeout and the end of the input
method.

`ime_candidates` writes `ime_page_size=2`, starts `imed -t` and gedit and
composes `abc`. It finds the selected candidate by its colour below the
caret and clicks the candidate to its right, which commits abc. It
composes `de`, turns to the second page with the wheel and chooses De with
Space. With `ime_orientation=vertical` it composes `fg` and clicks the
candidate below the selected one. gedit must save abcDefg.

`ime_pinyin` starts the compositor, `imed` and gedit and selects the
pinyin engine with a Shift tap. It types `zhongguo` and `nihao` with Space,
a full-width comma, the sentence `jintiantianqihenhao`, `zg`, which offers
中国 first after the first choice, `woaibeijing` with 2 for 我爱 and Space
for 北京, `zhongguox` with Backspace, `shi` with `=` and 1 for the sixth
candidate 式, `shi` again, which offers 式 first, `hello` with Enter, and
`shu` followed by a Shift tap. gedit must save
中国你好，今天天气很好中国我爱北京中国式式helloshux, and the user dictionary
must contain the line of 式. The host test `user/imed/tests/test_pinyin.c`
(`make check-imed`, part of `make check`) checks the candidates of words,
a sentence, an abbreviation and syllable divisions, and the learning.
