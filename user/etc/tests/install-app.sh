# Install the current image's build of the named app, then optionally run it.
name=$1
shift
/bin/pkg install /usr/share/packages/$name-*.mpk || exit 1
if test $# -gt 0; then
    /usr/bin/$name "$@"
fi
