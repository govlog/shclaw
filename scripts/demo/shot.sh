#!/bin/sh
# shot.sh <file.html|svg> <out.png> <width> <height>: render with headless Firefox
set -e
# A snap Firefox cannot read /tmp: keep the profile under $HOME
P=$(mktemp -d "$HOME/.cache/shclaw-ffprof.XXXX")
firefox --headless -no-remote -profile "$P" --window-size="$3,$4" --screenshot "$(readlink -f "$2")" "file://$(readlink -f "$1")" >/dev/null 2>&1
rm -rf "$P"
