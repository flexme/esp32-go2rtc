#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
${CC:-cc} -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I main main/hevc.c tests/test_hevc.c -o "$task_tmp/test_hevc"
"$task_tmp/test_hevc"
${CC:-cc} -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I components/peer components/peer/rtp_repair.c tests/test_rtp_repair.c -o "$task_tmp/test_rtp_repair"
"$task_tmp/test_rtp_repair"
node tests/test_diagnostics.js
