# Input methods

Input methods compose text for the clients that use the text input
protocol, which are the text widgets of libgui. The input method daemon
`imed` follows the model of IBus (`docs/plan/ime.md`): it contains a pinyin
engine with the Rime dictionary rime-pinyin-simp and a Japanese engine with
the dictionary of Mozc, and the compositor X12 relays between the text
input contexts and the daemon. `startgui` starts the daemon. The panel
shows the current method next to the mixer button. Without the daemon the
compositor composes only with its Ctrl+Shift+U entry and the dead keys of
the layout. The Region and language page of Settings chooses the engines
and the switch keys, and the terminal composes as the text widgets do.

## Methods

`user/compositor/inputmethod.c` retains the list of methods that the switch
keys select. The keyboard layout is the first method. The engines of the
daemon follow in the order of its `set_engines` request: `pinyin`, then
`japanese`. Until the user selects an engine, the first toggle selects the
first engine whose name contains `pinyin` or `chinese`. `im_select` selects a
method, `im_toggle` toggles between the layout and the last engine, and the
settings key `input_method` of the panel and the `ime_control` interface
select a method as well.

## Switch keys

The switch keys work on a macOS host, where Cmd+Space opens Spotlight and
Ctrl+Space switches the input source of macOS before QEMU sees them.

| Keys | Effect |
|---|---|
| a Shift tap | toggles between the layout and the last engine (the pinyin engine at first) |
| a Ctrl+Shift tap | selects the next method in the order layout, pinyin, Japanese |
| Ctrl+Space, Super+Space | toggle as a Shift tap does, where the host passes them |
| Zenkaku/Hankaku | toggles between the layout and the Japanese engine |
| Katakana/Hiragana, Henkan, Kana (LANG1) | select the Japanese engine, and then go to it |
| Eisu (LANG2) | selects the layout |
| the menu of the panel label | selects a method |

A tap is a press and a release without another key between them. Shift
with a letter therefore types a capital, and Ctrl+Shift+U still starts the
Unicode entry. A change of method commits the composition as it is shown.
`seat.c` detects the taps. The settings `ime_shift_toggle` and
`ime_ctrl_space` of `desktop.conf` (1 by default) turn the Shift tap and
Ctrl+Space with Super+Space on and off: the desktop sends them to the
compositor as settings of the same names. The settings key `input_method`
of the compositor selects a method by number, or the next one with -1.
The Kana and Eisu keys of Mac keyboards arrive as KEY_HANGEUL (122) and
KEY_HANJA (123) from the virtio keyboard, and as the scancodes 0xf2 and
0xf1, a press without a release, from a PS/2 keyboard.

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
contains the key in a queue of 32 entries until `key_handled` arrives. A used
key and its release do not reach the client. A passed key goes the usual
way through the dead keys of `text.c` to the client, with the modifiers of
the moment it was typed. A key without a reply in 1000 ms passes, and the
compositor logs a timeout. The keys typed after a waiting key wait behind
it, unsent, and the order remains. The switch keys and the Japanese keys wait
in the same queue: a switch typed after letters takes effect after the
daemon has answered for the letters, and the keys typed after a waiting
switch go where the switch sends them. A key with Ctrl, Alt or Super goes
to the client at once and to the daemon with serial 0, and the daemon
commits its composition. The daemon sends the changes of a key before the reply, and
the compositor therefore applies the text before it passes the key.

A key that passes after the timeout and arrives late at the daemon reaches
both the client and the engine. The timeout therefore only protects against
a daemon that no longer answers. The limit was 150 ms until 2026-10-06. The
first conversion of the Japanese engine took 194 ms under TCG, and the
tests `ime_japanese` and `ime_pinyin` then received a space in the text and
keys in the wrong order.

The `key` event carries no modifiers. The daemon applies the modifiers of
the last `modifiers` event to each key. While keys wait in the queue, the
compositor therefore defers the changes of the modifiers. It sends a
waiting key after the modifiers that were down when the key was typed, and
it sends the current modifiers once the queue is empty. Before 2026-10-06
the compositor sent each change at once. A Shift+Right typed while a
conversion waited for its reply then reached the daemon after the release
of Shift, as a plain Right arrow.

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
imed_engine` with `init`, `select`, `key`, `flush`, `reset` and
`candidate_clicked`. The daemon announces its engines first and then calls
`init`, which maps the dictionaries, and loads the CJK font. The switch
keys therefore find the engines while the daemon starts. It replies to a key before it
draws the candidate window and logs a key that took the engine more than
100 ms. An engine changes the composition through
`imed_commit`, `imed_preedit` and the lookup table `imed_table`, and the
daemon sends the changes after each event. `imed -t` adds the test engine
of the boot tests: letters compose, the candidates are the letters in
capitals, in small letters and with a capital first, and F12 replies after
1500 ms, after the timeout of the compositor.

## Dictionaries

`tools/fetch_imedata.sh` downloads the dictionaries of the engines at
pinned commits into `third_party/imedata`, which is not in the repository.
`tools/genime.py` writes the files of `user/share/ime` from them, which are
checked in and installed as `/usr/share/ime`. The daemon maps both
dictionaries with `mmap`, and a page is read when a lookup first touches
it.

`pinyin.dict` comes from `pinyin_simp.dict.yaml` of rime-pinyin-simp
(Apache-2.0, its license is installed beside it as
`pinyin_simp.LICENSE`): 415 syllables and 65125 entries of one to four
syllables with weights, 17000 characters and 48000 words of simplified
Chinese, ü written as v. The file is little endian. A header with the
magic `MPY1` gives the number of syllables and entries, the offsets of the
syllable table, the entry table and the pool, and the sum of the weights. The syllables are in
alphabetical order, 8 bytes each. An entry has 16 bytes: four syllable
numbers (0xffff after the last), the weight and the offset of the word in
the pool of zero-terminated UTF-8 words. The entries are sorted by their
syllable numbers. Because the syllables are sorted, the syllables that
begin with some letters have consecutive numbers, so an incomplete
syllable or an initial stands for a range of numbers.

`japanese.dict` comes from `dictionary_oss` of Mozc (the IPAdic license,
public domain and BSD 3-clause, whose README is installed beside it as
`mozc.README.txt`): 225811 entries of 138863 readings, 9.3 MB. The 2672
part of speech ids of Mozc become 1427 classes. A verb or an adjective
without a word of its own is grouped by its group, conjugation type and
conjugation form, and every other id remains a class: particles, auxiliary
verbs, nouns, and verbs with a word such as いる or 来る. The conjugation
type decides between た and だ after a verb, and the ids of single words
separate forms such as the hiragana わたし, which Mozc gives cost 0 and
suppresses through the connection costs of its id. The connection cost
between two classes is the mean of the costs between their ids, stored in
steps of 60 as one byte. The dictionary has the entries with a cost below
6000 and every particle and auxiliary verb. A header with the magic `MJP1` gives
the numbers of classes, readings and entries, the class of the start and
end of a sentence and of common nouns, and the offsets of the class flags,
the matrix, the readings, the entries and the pool. A class flag marks a
particle or auxiliary verb, a suffix or dependent word, or a prefix. The
readings are sorted by bytes, 8 bytes each: the offset of the reading and
the number of its first entry. An entry has 10 bytes: the offset of the
surface (or a mark for the reading itself or the reading in katakana), the
left and right classes and the cost.

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

## Japanese engine

The Japanese engine of `imed` (`user/imed/japanese.c` with the core
`jpcore.c` and `romaji.c`) follows the keys of Mozc and MS-IME. Its label
is あ.

Romaji become hiragana while the letters are typed. A sequence that begins
a longer sequence waits, a doubled consonant gives a small っ, an n before a
consonant gives ん, and `-`, `,`, `.`, `[`, `]` give ー, 、, 。, 「, 」.

The core makes a node of every part of the reading that is a reading of
the dictionary or of the user history, with the cost of its word, and a
node of every character alone with a cost of 15000. A path therefore
always exists. A Viterbi search finds the path with the lowest sum of word costs
and connection costs between neighbouring classes, from the start of the
sentence to its end. The path falls into segments: a word that is not a
particle, an auxiliary verb, a suffix or a dependent word starts a segment,
unless a prefix comes before it. わたしはがくせいです becomes 私は｜学生です
and かのじょはとしょかんでほんをよんだ becomes 彼女は｜図書館で｜本を｜読んだ.
When a segment is resized, its part of the reading is converted alone and
the rest again. The candidates of a segment are the lowest cost path of its
reading, the words of its whole reading (chosen ones first), the other
words of its first word followed by the rest, then hiragana and katakana.

| Key | Composing | Converting |
|---|---|---|
| letters | compose | commit, then compose |
| Space, Henkan | convert | the next candidate, and the second Space opens the candidates |
| Up, Down | | the previous or the next candidate |
| 1 to 9 | | choose on the page and go to the next segment |
| Left, Right | | the previous or the next segment |
| Shift+Left, Shift+Right | | shorten or lengthen the current segment |
| Enter | commit the kana | commit the segments |
| Escape | drop | back to the kana |
| Backspace | delete the last letter or kana | back to the kana |
| F6 to F10 | hiragana, katakana, half-width katakana, full-width letters, letters as typed | F6 to F8 for the current segment |
| Muhenkan | toggle katakana | |

The auxiliary line of the candidate window shows the segments with the
current one in brackets, 【今日は】いい天気ですね. A segment whose candidate
was changed is learned in `$HOME/.config/imed/japanese.user`: one line
`reading TAB surface TAB left class TAB right class TAB count` per choice.
A learned surface becomes a node of cost 2500 less 300 for each choice, up
to 8. After 漢字 was chosen for かんじ, かんじ converts to 漢字.

## Caret

The caret comes from the request `set_cursor_rectangle` of `text_input`
version 2, in surface coordinates, applied at the next commit. The editor
and the text field of libgui report the start of the preedit through
`widget_text_cursor`, and libgui sends only a changed rectangle. Without a
rectangle the candidate window appears near the top left corner of the
surface.

## Panel menu

A click on the label of the panel (`user/panel/imemenu.c`) opens a popup
above it with one row per method: its label and its title, the keyboard
layout first, and a highlight on the current one. The panel binds the
`input_method_manager` global and gets an `ime_control`, whose `engines`
and `current` events give the rows. A click on a row sends `select` with
its name, and the compositor selects it as a switch key does. The titles of
the known methods are translated in the `panel` domain.

## Settings

The Region and language page of Settings (`user/settings/region.c`) has
an Input methods section:

| Control | Configuration key |
|---|---|
| a checkbox per engine, with Move up | `ime_engines`: the enabled engines in order, `pinyin,japanese` by default |
| A Shift tap toggles the input method | `ime_shift_toggle` |
| Ctrl+Space toggles the input method | `ime_ctrl_space` |
| Candidates per page (2 to 9) | `ime_page_size` |
| Candidate layout | `ime_orientation`: `horizontal` or `vertical` |

The daemon reads `desktop.conf` again when it changes. It announces the
engines of `ime_engines` in their order within a second, and a disabled
engine leaves the switch keys and the panel menu. The page size and the
orientation apply at the next key.

## Terminal

The canvas widget of libgui emits the signals `text` and `preedit` when it
accepts text, and the terminal (`user/term/term.c`) sets `accepts_text` on
its canvas. The text of the printable keys then arrives as committed text,
from the dead keys of the compositor or from an engine, and the terminal
writes it to the pty. Keys with Ctrl or Alt, the cursor keys and the
function keys remain key events, which the terminal turns into bytes as
before. The preedit covers the cells from the cursor, underlined, and the
terminal reports the cursor cell as the caret, where the candidate window
appears.

## Label

The seat of version 2 has the event `input_method` with a short label: the
label of the engine (拼 for the pinyin engine, あ for the Japanese engine),
and otherwise the layout name in capitals. The `us` layout and the first
group of a layout with two groups are EN. The compositor sends the label after the bind,
after a change of method, after Alt+Shift changes the group and after a
keymap reload. The panel binds the seat with a listener and draws the label.

## Limits

The pinyin dictionary has words of at most four syllables, and longer text
comes from sentences. The Japanese engine has no bigram costs between
words and no prediction. かんじへんかん therefore becomes 感じ変換, where
Mozc gives 漢字変換. The pinyin engine has no fuzzy syllables (zh for z, ing for in)
and neither engine has a caret inside the input. The compositor repeats no
key for the daemon: a Backspace that remains down deletes one letter of the
input.
The engines compose only in clients with text input enabled: the text
widgets of libgui and the terminal.

## Test

`ime` starts the compositor, `imed`, the panel and gedit. A Ctrl+Shift tap
selects the pinyin engine for `nihao`, another one the Japanese engine for
`nihongo`, a Shift tap the layout for `a`, another one the Japanese engine
for `yama`, Ctrl+Space the layout for `b`, Zenkaku/Hankaku the Japanese
engine for `hashi` with Enter, and the Eisu key the layout for `c`. gedit
must save 你好日本語a山bはしc, which also shows that no Space or Enter
reached gedit as a key. The second row of the panel menu must then select
the pinyin engine, and the compositor log must report the labels.

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

`ime_japanese` starts the compositor, `imed` and gedit and selects the
Japanese engine with two Ctrl+Shift taps. It converts `nihongo`,
`watashihagakuseidesu` and `kyouhaiitenkidesune`, lengthens the first
segment of the last with Shift+Right, chooses the fourth candidate of
`kanji` with a second Space and 4, converts `kanji` again, which gives the
learned 漢字, commits `katakana` after F7, and returns `yama` to the kana
with Escape. gedit must save 日本語私は学生です今日はい移転機ですね漢字漢字カタカナやま,
and the user history must contain 漢字 for かんじ. The host test
`user/imed/tests/test_japanese.c` (part of `make check-imed`) checks the
conversion of words and sentences, the learning and the forms.

`gui_ime_settings` starts the compositor and the panel, `imed` and the
terminal, and sets `ime_page_size` to 3 with `settings set`. In the
terminal the layout types `echo `, the panel menu selects the pinyin
engine, which composes 你好, ignores the digit 4 for `shi`, because the
page has 3 candidates, and chooses 是 with Space. A Shift tap selects the
layout for ` >/imeterm` and Enter. The file must contain 你好是. `settings
set ime_engines japanese` must leave the daemon with one engine, and the
panel menu must offer two methods and select the Japanese engine.
