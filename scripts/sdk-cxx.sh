#!/usr/bin/env bash
set -euo pipefail
novelty_sdk=${NOVELTY_SDK_PATH:-$HOME/.local/share/flatpak/runtime/org.freedesktop.Sdk/x86_64/25.08/active/files}
novelty_project=${NOVELTY_PROJECT_PATH:-$(cd -- "$(dirname -- "$0")/.." && pwd)}
exec "$novelty_sdk/bin/g++" --sysroot="$novelty_project/.tools/sysroot" \
  -B /usr/bin/ -B "$novelty_sdk/lib/x86_64-linux-gnu/" -L/usr/lib \
  -Wl,-rpath-link,/usr/lib/libproxy "$@"
