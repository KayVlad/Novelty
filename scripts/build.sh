#!/usr/bin/env bash
set -euo pipefail
novelty_project=$(cd -- "$(dirname -- "$0")/.." && pwd)
if command -v cmake >/dev/null && command -v c++ >/dev/null; then
  cmake -S "$novelty_project" -B "$novelty_project/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$novelty_project/build" --parallel 2
  ctest --test-dir "$novelty_project/build" --output-on-failure
else
  novelty_sdk=${NOVELTY_SDK:-$HOME/.local/share/flatpak/runtime/org.freedesktop.Sdk/x86_64/25.08/active/files}
  if [[ ! -x "$novelty_sdk/bin/g++" || ! -f "$novelty_project/.tools/qt-include/QtWidgets/qapplication.h" ]]; then
    echo 'Install CMake, a C++20 compiler and Qt6 Core/Widgets/Sql/Network/DBus development packages in your build environment.' >&2
    echo 'The local Deck fallback additionally needs scripts/bootstrap-qt-headers.py.' >&2
    exit 1
  fi
  mkdir -p "$novelty_project/.tools/sysroot" "$novelty_project/build"
  if [[ ! -e "$novelty_project/.tools/sysroot/usr" ]]; then ln -s "$novelty_sdk" "$novelty_project/.tools/sysroot/usr"; fi
  export NOVELTY_SDK_PATH="$novelty_sdk"
  export NOVELTY_PROJECT_PATH="$novelty_project"
  "$novelty_sdk/bin/cmake" -S "$novelty_project" -B "$novelty_project/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNOVELTY_LOCAL_QT=ON \
    -DCMAKE_CXX_COMPILER="$novelty_project/scripts/sdk-cxx.sh" \
    -DCMAKE_MAKE_PROGRAM="$novelty_sdk/bin/ninja"
  "$novelty_sdk/bin/cmake" --build "$novelty_project/build" --parallel 2
  "$novelty_sdk/bin/ctest" --test-dir "$novelty_project/build" --output-on-failure
fi
