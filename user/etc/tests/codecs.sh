# The codecs command (docs/design/codecs.md), run as: sh /etc/tests/codecs.sh.
# Every failing check prints a line that starts with FAIL and shows the
# value received. The last line reports the end of the script.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /ct
cd /ct

# The registry: the modules with their codecs and capabilities.
codecs > list.txt; check list-status "$?" "0"
check list-total "$(tail -n 1 list.txt)" "11 codecs in 10 modules"
check list-oggflac "$(grep -c '^DE-- oggflac audio  flac.so' list.txt)" "1"
check list-vorbis "$(grep -c '^DE-- vorbis audio  vorbis.so' list.txt)" "1"
check list-opus "$(grep -c '^D--- opus  audio  opus.so' list.txt)" "1"
check list-flac "$(grep -c '^DE-- flac  audio  flac.so' list.txt)" "1"
check list-bmp "$(grep -c '^DE-- bmp   image  bmp.so' list.txt)" "1"
check list-gif "$(grep -c '^DE-A gif   image  gif.so' list.txt)" "1"
check list-jpeg "$(grep -c '^DE-- jpeg  image  jpeg.so' list.txt)" "1"
check list-mp3 "$(grep -c '^DE-- mp3   audio  mp3.so' list.txt)" "1"
check list-png "$(grep -c '^DE-- png   image  png.so' list.txt)" "1"
check list-svg "$(grep -c '^D-S- svg   image  svg.so' list.txt)" "1"
check list-wav "$(grep -c '^DE-- wav   audio  wav.so' list.txt)" "1"

# PNG to BMP and back: the same pixels, so the two BMP files are equal.
codecs convert /usr/share/icons/folder.png f.bmp > out.txt; check to-bmp "$?" "0"
check to-bmp-line "$(cat out.txt)" "/usr/share/icons/folder.png (png) -> f.bmp (bmp)"
check info-bmp "$(codecs info f.bmp)" "f.bmp: bmp image, 16x16, with alpha"
codecs convert f.bmp f.png > /dev/null; check to-png "$?" "0"
codecs convert f.png g.bmp > /dev/null
cmp f.bmp g.bmp; check same-pixels "$?" "0"
check info-png "$(codecs info f.png)" "f.png: png image, 16x16, with alpha"
# PNG to JPEG and back: JPEG has no alpha and loses detail, which leaves
# the size.
codecs convert f.png f.jpg > /dev/null; check to-jpeg "$?" "0"
check info-jpeg "$(codecs info f.jpg)" "f.jpg: jpeg image, 16x16"
codecs convert f.jpg j.png > /dev/null; check from-jpeg "$?" "0"
check info-from-jpeg "$(codecs info j.png)" "j.png: png image, 16x16"
# The content decides, not the name.
cp f.bmp misnamed.png
check content-wins "$(codecs info misnamed.png)" "misnamed.png: bmp image, 16x16, with alpha"
codecs convert -f bmp f.png named > /dev/null; check by-name "$?" "0"
cmp named f.bmp; check by-name-same "$?" "0"

# A vector image renders at -s.
printf '<svg viewBox="0 0 8 8"><path d="M0 0H8V8H0Z" fill="#ff0000"/></svg>' > square.svg
check info-svg "$(codecs info square.svg)" "square.svg: svg image, 256x256, scalable"
codecs convert -s 24 square.svg square.png > /dev/null; check svg-render "$?" "0"
check info-svg-png "$(codecs info square.png)" "square.png: png image, 24x24"

# Audio: the chime at 24 bits and back to 16 is the chime again.
check info-wav "$(codecs info /usr/share/sounds/chime.wav)" \
    "/usr/share/sounds/chime.wav: wav audio, 16000 Hz, 1 channel, 16 bit, 32000 frames, 2.000 s"
codecs convert -b 24 /usr/share/sounds/chime.wav c24.wav > /dev/null; check wav-24 "$?" "0"
check info-24 "$(codecs info c24.wav)" "c24.wav: wav audio, 16000 Hz, 1 channel, 24 bit, 32000 frames, 2.000 s"
codecs convert -b 16 c24.wav c16.wav > /dev/null
codecs convert /usr/share/sounds/chime.wav same.wav > /dev/null
cmp c16.wav same.wav; check wav-lossless "$?" "0"

# FLAC: the chime compressed and expanded again is the same WAV file.
codecs convert same.wav c.flac > /dev/null; check to-flac "$?" "0"
check info-flac "$(codecs info c.flac)" "c.flac: flac audio, 16000 Hz, 1 channel, 16 bit, 32000 frames, 2.000 s"
codecs convert c.flac back.wav > /dev/null; check from-flac "$?" "0"
cmp back.wav same.wav; check flac-lossless "$?" "0"
codecs convert -b 24 c.flac c24.flac > /dev/null
check info-flac24 "$(codecs info c24.flac | cut -d, -f4)" " 24 bit"
check info-shipped "$(codecs info /usr/share/sounds/chime.flac | cut -d: -f2)" " flac audio, 16000 Hz, 1 channel, 16 bit, 32000 frames, 2.000 s"

# Vorbis: identified by content, decoded to WAV and FLAC.
check info-vorbis "$(codecs info /usr/share/sounds/chime.ogg | cut -d: -f2)" " vorbis audio, 16000 Hz, 1 channel, 32000 frames, 2.000 s"
codecs convert /usr/share/sounds/chime.ogg v.wav > /dev/null; check vorbis-to-wav "$?" "0"
check info-vorbis-wav "$(codecs info v.wav)" "v.wav: wav audio, 16000 Hz, 1 channel, 16 bit, 32000 frames, 2.000 s"
codecs convert /usr/share/sounds/chime.ogg v.flac > /dev/null; check vorbis-to-flac "$?" "0"

# FLAC in Ogg: .oga selects it, and the round trip is lossless.
codecs convert same.wav c.oga > out.txt; check to-oga "$?" "0"
check to-oga-line "$(cat out.txt)" "same.wav (wav) -> c.oga (oggflac)"
codecs convert c.oga back2.wav > /dev/null
cmp back2.wav same.wav; check oga-lossless "$?" "0"

# Encoder options: the quality of Vorbis, and none for FLAC.
codecs convert -o quality=0.0 same.wav low.ogg > /dev/null; check quality-low "$?" "0"
codecs convert -o quality=0.9 same.wav high.ogg > /dev/null; check quality-high "$?" "0"
check quality-size "$(test $(wc -c < high.ogg) -gt $(wc -c < low.ogg) && echo larger)" "larger"
check info-encoded "$(codecs info high.ogg | cut -d: -f2)" " vorbis audio, 16000 Hz, 1 channel, 32000 frames, 2.000 s"
codecs convert -o quality=2 same.wav bad.ogg 2> err.txt; check quality-range "$?" "1"
codecs convert -o quality=1 same.wav x.flac 2> err.txt; check flac-options "$?" "1"
check flac-options-text "$(cat err.txt)" "codecs: x.flac: cannot encode: Invalid argument"

# GIF: a still image with alpha, and an animation that remains one.
codecs convert f.png f.gif > /dev/null; check to-gif "$?" "0"
check info-gif "$(codecs info f.gif)" "f.gif: gif image, 16x16, with alpha"
codecs convert f.gif fg.bmp > /dev/null; check from-gif "$?" "0"
check info-from-gif "$(codecs info fg.bmp)" "fg.bmp: bmp image, 16x16, with alpha"
check info-anim "$(codecs info /etc/tests/codec-gif-pillow.gif | cut -d: -f2)" \
    " gif image, 48x32, with alpha, 4 frames, 1.100 s, plays 3 times"
codecs convert /etc/tests/codec-gif-pillow.gif a.gif > /dev/null; check anim-gif "$?" "0"
check info-anim-copy "$(codecs info a.gif)" "a.gif: gif image, 48x32, with alpha, 4 frames, 1.100 s, plays 3 times"
codecs convert /etc/tests/codec-gif-previous.gif p.png > /dev/null; check anim-first-frame "$?" "0"
check info-first-frame "$(codecs info p.png)" "p.png: png image, 40x30"

# MP3: the chime through the encoder of MPEG-2 has its original length,
# and the fixture of LAME has the length that its tag gives.
codecs convert same.wav c.mp3 > /dev/null; check to-mp3 "$?" "0"
check info-mp3 "$(codecs info c.mp3)" "c.mp3: mp3 audio, 16000 Hz, 1 channel, 32000 frames, 2.000 s"
codecs convert c.mp3 m.wav > /dev/null; check from-mp3 "$?" "0"
check info-mp3-wav "$(codecs info m.wav)" "m.wav: wav audio, 16000 Hz, 1 channel, 16 bit, 32000 frames, 2.000 s"
codecs convert -o bitrate=64 same.wav c64.mp3 > /dev/null; check mp3-bitrate "$?" "0"
check mp3-size "$(test $(wc -c < c64.mp3) -gt $(wc -c < c.mp3) && echo larger)" "larger"
codecs convert -o bitrate=100 same.wav bad.mp3 2> err.txt; check mp3-bad-bitrate "$?" "1"
check info-lame "$(codecs info /etc/tests/codec-mp3-lame.mp3 | cut -d: -f2)" \
    " mp3 audio, 44100 Hz, 2 channels, 66150 frames, 1.500 s"

# Errors.
echo "plain text" > note.txt
codecs info note.txt > out.txt; check unknown-status "$?" "1"
check unknown-line "$(cat out.txt)" "note.txt: unknown format"
codecs convert f.png f.svg 2> err.txt; check no-encoder "$?" "1"
check no-encoder-text "$(cat err.txt)" "codecs: f.svg: no encoder for the extension"
codecs convert f.png f.wav 2> /dev/null; check other-kind-ext "$?" "1"
codecs convert -f wav f.png x 2> err.txt; check other-kind "$?" "1"
check other-kind-text "$(cat err.txt)" "codecs: x: the formats are of different kinds"
codecs convert f.png 2> /dev/null; check usage "$?" "2"
codecs info missing.png 2> err.txt; check missing "$?" "1"
check missing-text "$(cat err.txt)" "codecs: missing.png: No such file or directory"

# The extensions the MIME table gives the formats.
check mime "$(grep -c '^image/bmp bmp dib$' /etc/mime.types)" "1"
check mime-gif "$(grep -c '^image/gif gif$' /etc/mime.types)" "1"
check mime-mp3 "$(grep -c '^audio/mpeg mp3$' /etc/mime.types)" "1"
echo "codecs: done"
