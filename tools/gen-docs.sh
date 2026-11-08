#!/bin/sh
# Regenerate the option tables in README.md and man/tcpser.1 from the option
# table compiled into the binary (src/options.c), so the three never drift.
#
#   sh tools/gen-docs.sh ./tcpser            rewrite the files in place
#   sh tools/gen-docs.sh ./tcpser --check    exit 1 if they are out of date
#
# The generated text lives between marker lines:
#   README.md:    <!-- options:begin -->  ...  <!-- options:end -->
#   man/tcpser.1: .\" options:begin       ...  .\" options:end
set -eu
BIN=${1:-./tcpser}
MODE=${2:-}
TMP=${TMPDIR:-/tmp}/tcpser-gen-docs.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

# splice BEGIN..END markers in FILE with the contents of BODY
splice() {   # file begin end body   (markers via ENVIRON: awk -v would eat the backslash in .\")
  B="$2" E="$3" BODY="$4" awk '
    BEGIN { b = ENVIRON["B"]; e = ENVIRON["E"]; body = ENVIRON["BODY"] }
    index($0, b) == 1 { print; while((getline l < body) > 0) print l; close(body); skip = 1; next }
    index($0, e) == 1 { skip = 0 }
    !skip { print }
  ' "$1" > "$TMP/out" && cat "$TMP/out"
}

"$BIN" help markdown > "$TMP/md"
"$BIN" help man > "$TMP/man"
splice README.md '<!-- options:begin -->' '<!-- options:end -->' "$TMP/md" > "$TMP/README.md"
splice man/tcpser.1 '.\" options:begin' '.\" options:end' "$TMP/man" > "$TMP/tcpser.1"

rc=0
for pair in "README.md $TMP/README.md" "man/tcpser.1 $TMP/tcpser.1"; do
  set -- $pair
  if ! cmp -s "$1" "$2"; then
    if [ "$MODE" = "--check" ]; then
      echo "gen-docs: $1 is out of date (run: make docs)"; rc=1
    else
      cp "$2" "$1"; echo "gen-docs: updated $1"
    fi
  fi
done
exit $rc
