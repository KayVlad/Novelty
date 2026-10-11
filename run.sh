#!/usr/bin/env bash
set -euo pipefail
novelty_project=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ ! -x "$novelty_project/build/novelty" ]]; then
  echo 'Build Novelty first with ./scripts/build.sh' >&2
  exit 1
fi
exec "$novelty_project/build/novelty" "$@"
