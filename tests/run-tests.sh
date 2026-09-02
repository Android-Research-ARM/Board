#!/bin/zsh
# board/tests/run-tests.sh, the .brd format's own test suite.
#
#   board/tests/run-tests.sh [path-to-board-binary]
#
# two halves:
#   1. round-trip. `board create` writes a file, `board inspect` reads back
#      exactly what went in, `board validate` accepts it.
#   2. rejection. tests/forge_brd.py writes every case it knows (run
#      `forge_brd.py list`), deliberately malformed or hostile files,
#      including three that try to smuggle a technical field (soc model,
#      ro.hardware, a board name) past the parser under an unrecognised field
#      id. every one but the `valid` control must be refused with a nonzero
#      exit. deliberately not a hardcoded count: adding a case should not
#      require editing a number in a comment.
#
# working files go in a mktemp -d: they are per-run and nothing later reuses
# them, so there is nothing here worth keeping.

set -euo pipefail

board="${1:-$(cd "$(dirname "$0")/../.." && pwd)/Build/Products/board}"
forge="$(cd "$(dirname "$0")" && pwd)/forge_brd.py"

if [[ ! -x "$board" ]]; then
  print -u2 "run-tests.sh: no board binary at $board, run 'make' in Board/ first"
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

passed=0
failed=0

ok()   { print "  PASS  $1"; (( ++passed )) || true }
bad()  { print "  FAIL  $1"; (( ++failed )) || true }

print "== round trip =="

"$board" create \
  --brand pocket --manufacturer Pocket --model POCKET_P1_A \
  --codename POCKET_P1 --name "Pocket P1" --author "test-suite" \
  --output "$work/good.brd" >/dev/null

[[ -f "$work/good.brd" ]] && ok "create writes a file" || bad "create writes a file"

# the format is fixed-shape, so a valid file's size is predictable: 16 header +
# 6*(3 + len) + 4 crc. asserting it catches an accidental layout change that
# still round-trips.
size=$(stat -f %z "$work/good.brd")
expected=$(( 16 + (3+6) + (3+6) + (3+11) + (3+9) + (3+9) + (3+10) + 4 ))
[[ "$size" == "$expected" ]] && ok "file is $size bytes, the exact expected layout" \
                             || bad "file is $size bytes, expected $expected"

head -c 8 "$work/good.brd" | grep -q '^ARASBRD1$' && ok "magic bytes are ARASBRD1" \
                                                  || bad "magic bytes are ARASBRD1"

# not plain text: a binary format that a text editor would not invite you to
# edit is the actual design goal, so assert it really is binary.
if grep -qI . "$work/good.brd" 2>/dev/null; then
  bad "file is binary, not text"
else
  ok "file is binary, not text"
fi

"$board" validate "$work/good.brd" >/dev/null && ok "validate accepts it" || bad "validate accepts it"

inspect_out="$("$board" inspect "$work/good.brd")"
for pair in "brand         pocket" "manufacturer  Pocket" "model         POCKET_P1_A" \
            "codename      POCKET_P1" "device name   Pocket P1"; do
  if print -r -- "$inspect_out" | grep -q "  $pair"; then ok "inspect reports ${pair%% *}"
  else bad "inspect reports ${pair%% *}"; fi
done

if print -r -- "$inspect_out" | grep -q "author        test-suite   (self-reported, not verified)"; then
  ok "inspect labels the author self-reported and unverified"
else
  bad "inspect labels the author self-reported and unverified"
fi

print "\n== input rejection at create time =="

# create must refuse a bad value rather than writing a file the host app
# would then reject.
if "$board" create --brand "not a token" --manufacturer P --model M --codename C \
      --name N --author A --output "$work/never.brd" >/dev/null 2>&1; then
  bad "create rejects a brand with spaces"
else
  [[ -f "$work/never.brd" ]] && bad "create wrote a file it should have refused" \
                             || ok "create rejects a brand with spaces, writes nothing"
fi

if "$board" create --brand p --manufacturer P --model M --codename C \
      --name N --author "" --output "$work/never2.brd" >/dev/null 2>&1; then
  bad "create rejects a blank author"
else
  ok "create rejects a blank author"
fi

print "\n== hostile and malformed files =="

# note: not `path=`, in zsh `path` is tied to the PATH array, and assigning a
# string to it empties PATH for the rest of the script (including the EXIT trap).
for name in $(python3 "$forge" list); do
  file="$work/$name.brd"
  python3 "$forge" "$name" "$file"
  if [[ "$name" == "valid" ]]; then
    "$board" validate "$file" >/dev/null 2>&1 \
      && ok "valid: accepted (control)" || bad "valid: REJECTED, the control case must pass"
    continue
  fi
  if "$board" validate "$file" >/dev/null 2>&1; then
    bad "$name: ACCEPTED, must have been rejected"
  else
    # `|| true` because validate exits 1 here by design, and with `set -o
    # pipefail` the assignment would otherwise inherit that and trip `set -e`.
    # tolerate either separator after INVALID, the CLI's wording has changed
    # once already and a cosmetic reword should not make this line print the
    # whole path back at you.
    reason="$({ "$board" validate "$file" 2>&1 || true; } | sed -E 's/.*INVALID( —|:) //')"
    ok "$name: rejected ($reason)"
  fi
  # inspect must refuse it too: the host app shows the user a file's contents
  # through the same code path before installing, so a file that validate
  # rejects must never be renderable as if it were fine.
  # `cmd && bad ...` would return nonzero when cmd fails, and `set -e` would
  # kill the run on the first correctly-rejected file. use an explicit if.
  if "$board" inspect "$file" >/dev/null 2>&1; then
    bad "$name: inspect displayed an invalid file"
  fi
done

print "\n$passed passed, $failed failed"
[[ "$failed" == 0 ]]
