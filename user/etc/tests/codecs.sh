# The codecs command (docs/design/codecs.md), run as: sh /etc/tests/codecs.sh.
# Every failing check prints a line that starts with FAIL and shows the
# value received. The last line reports the end of the script.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /ct
cd /ct

# The registry: four modules, one codec each, with their capabilities.
codecs > list.txt; check list-status "$?" "0"
check list-total "$(tail -n 1 list.txt)" "6 codecs in 6 modules"
check list-vorbis "$(grep -c '^D-- vorbis audio  vorbis.so' list.txt)" "1"
check list-flac "$(grep -c '^DE- flac  audio  flac.so' list.txt)" "1"
check list-bmp "$(grep -c '^DE- bmp   image  bmp.so' list.txt)" "1"
check list-png "$(grep -c '^DE- png   image  png.so' list.txt)" "1"
check list-svg "$(grep -c '^D-S svg   image  svg.so' list.txt)" "1"
check list-wav "$(grep -c '^DE- wav   audio  wav.so' list.txt)" "1"

# PNG to BMP and back: the same pixels, so the two BMP files are equal.
codecs convert /usr/share/icons/folder.png f.bmp > out.txt; check to-bmp "$?" "0"
check to-bmp-line "$(cat out.txt)" "/usr/share/icons/folder.png (png) -> f.bmp (bmp)"
check info-bmp "$(codecs info f.bmp)" "f.bmp: bmp image, 16x16, with alpha"
codecs convert f.bmp f.png > /dev/null; check to-png "$?" "0"
codecs convert f.png g.bmp > /dev/null
cmp f.bmp g.bmp; check same-pixels "$?" "0"
check info-png "$(codecs info f.png)" "f.png: png image, 16x16, with alpha"
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
echo "codecs: done"
