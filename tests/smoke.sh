#!/bin/sh
# Boots the Dragon 32 ROM, types a few BASIC lines and checks the answers.
# Needs your own ROM image: ./tests/smoke.sh [path/to/d32.rom]
set -e
cd "$(dirname "$0")/.."
ROM="${1:-d32.rom}"
if [ ! -f "$ROM" ]; then
    echo "SKIP: no ROM at $ROM (supply your own Dragon 32 ROM)"
    exit 0
fi
make -s
OUT=$(./emulator -q 4000000 --inject '? 2+3*4\r10 GOTO 10\rRUN\r\b' "$ROM")
fail=0
for want in "OK" " 14" "BREAK IN 10"; do
    if printf '%s\n' "$OUT" | grep -q "|$want"; then
        echo "PASS: screen shows '$want'"
    else
        echo "FAIL: screen does not show '$want'"; fail=1
    fi
done
[ $fail -eq 0 ] || { printf '%s\n' "$OUT" | tail -20; exit 1; }
