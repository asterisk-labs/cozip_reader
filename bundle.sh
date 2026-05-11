#!/usr/bin/env bash
set -euo pipefail

dump() {
  echo
  echo "═══ $1 ═══"
  [[ -f "$1" ]] && cat "$1" || echo "(not found)"
}

echo "═══ TREE ═══"
tree -L 2 -a -I '.git|duckdb|build|extension-ci-tools' 2>/dev/null \
  || find . -maxdepth 2 -not -path './.git*' -not -path './duckdb*' \
            -not -path './build*' -not -path './extension-ci-tools*' | sort

dump CMakeLists.txt
dump extension_config.cmake
dump Makefile
dump vcpkg.json
dump src/include/cozip_extension.hpp
dump src/cozip_extension.cpp