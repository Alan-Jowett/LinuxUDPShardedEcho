#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 LinuxUDPShardedEcho Contributors
set -euo pipefail

# Default: check staged or modified files. Use --all to check entire repo.
FIX=0
ALL=0
VERBOSE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fix) FIX=1; shift ;;
    --all|-a) ALL=1; shift ;;
    --verbose|-v) VERBOSE=1; shift ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT"

# Source canonical extensions list if present
if [ -f "$HERE/extensions.sh" ]; then
  # shellcheck disable=SC1090
  . "$HERE/extensions.sh"
fi


# File categories:
# - SRC_FILES: C/C++ sources/headers (clang-format + cppcheck)
# - SPDX_CANDIDATES: files that should contain an SPDX header (includes SRC_FILES)
if [ -n "${SRC_EXTENSIONS:-}" ]; then
  SRC_PATTERN=$(printf '\\.(%s)$$' "$(echo "$SRC_EXTENSIONS" | sed 's/ /|/g')")
else
  SRC_PATTERN='\\.(cpp|c|cc|cxx|h|hpp|hh|hxx)$'
fi

if [ -n "${SPDX_EXTENSIONS:-}" ]; then
  SPDX_PATTERN=$(printf '\\.(%s)$$' "$(echo "$SPDX_EXTENSIONS" | sed 's/ /|/g')")
else
  SPDX_PATTERN='\\.(cpp|c|cc|cxx|h|hpp|hh|hxx|cmake|ya?ml|md|ps1|sh|clang-format|txt)$'
fi

if [ "$ALL" -eq 1 ]; then
  SPDX_FILES=$(git -c core.safecrlf=false ls-files | grep -E "$SPDX_PATTERN" || true)
  SRC_FILES=$(git -c core.safecrlf=false ls-files | grep -E "$SRC_PATTERN" || true)
else
  STAGED_SP=$(git -c core.safecrlf=false diff --cached --name-only --diff-filter=ACM | grep -E "$SPDX_PATTERN" || true)
  MOD_SP=$(git -c core.safecrlf=false diff --name-only --diff-filter=ACM | grep -E "$SPDX_PATTERN" || true)
  SPDX_FILES=$(printf '%s\n%s' "$STAGED_SP" "$MOD_SP" | sed '/^$/d' | sort -u || true)

  STAGED_SRC=$(git -c core.safecrlf=false diff --cached --name-only --diff-filter=ACM | grep -E "$SRC_PATTERN" || true)
  MOD_SRC=$(git -c core.safecrlf=false diff --name-only --diff-filter=ACM | grep -E "$SRC_PATTERN" || true)
  SRC_FILES=$(printf '%s\n%s' "$STAGED_SRC" "$MOD_SRC" | sed '/^$/d' | sort -u || true)
fi

# Exclude generated expression outputs from checks
if [ -n "$SPDX_FILES" ]; then
  SPDX_FILES=$(printf '%s\n' "$SPDX_FILES" | grep -Ev '^(tests/regression_tests/expression_tree_|tests/regression_tests/expression_bdd_)' || true)
fi
if [ -n "$SRC_FILES" ]; then
  SRC_FILES=$(printf '%s\n' "$SRC_FILES" | grep -Ev '^(tests/regression_tests/expression_tree_|tests/regression_tests/expression_bdd_)' || true)
fi

if [ -z "$SPDX_FILES" ]; then
  if [ "$ALL" -eq 1 ]; then
    echo "No eligible files found in repository."
  else
    if [ "$VERBOSE" -eq 1 ]; then
      echo "No changes to check — OK"
    fi
  fi
  exit 0
fi

if [ "$ALL" -eq 1 ] || [ "$FIX" -eq 1 ] || [ "$VERBOSE" -eq 1 ]; then
  echo "SPDX-eligible files to check:"
  printf '%s\n' "$SPDX_FILES"
  echo
  if [ -n "$SRC_FILES" ]; then
    echo "C/C++ source files to check formatting/static analysis:";
    printf '%s\n' "$SRC_FILES"
  fi
fi

# SPDX check: ensure each eligible file has an SPDX marker within first 20 lines
MISSING_SPDX=()
for f in $SPDX_FILES; do
  if [ ! -f "$f" ]; then continue; fi
  if ! head -n 20 "$f" | grep -q "SPDX-License-Identifier"; then
    MISSING_SPDX+=("$f")
  fi
done

if [ ${#MISSING_SPDX[@]} -ne 0 ]; then
  echo "ERROR: The following files are missing an SPDX-License-Identifier in the first 20 lines:" >&2
  for m in "${MISSING_SPDX[@]}"; do echo "  $m" >&2; done
  echo "Add an SPDX header or run the contributor script to add headers." >&2
  exit 4
fi

# Run formatting for C/C++ files only
if [ -n "$SRC_FILES" ]; then
  if [ "$FIX" -eq 1 ]; then
    echo "Applying clang-format to source files..."
    if [ -n "$SRC_FILES" ]; then
      printf '%s\n' "$SRC_FILES" | xargs clang-format -i
    fi
  else
    if [ -n "$SRC_FILES" ]; then
      printf '%s\n' "$SRC_FILES" | xargs clang-format --dry-run --Werror || {
        echo "Formatting issues found. Re-run with --fix to apply clang-format." >&2
        exit 2
      }
    fi
  fi
else
  echo "No C/C++ source files selected for formatting/static analysis."
fi

# Run cppcheck on C/C++ files only if present
if [ -n "$SRC_FILES" ]; then
  if command -v cppcheck >/dev/null 2>&1; then
    echo "Running cppcheck..."
    if [ -n "$SRC_FILES" ]; then
      printf '%s\n' "$SRC_FILES" | xargs cppcheck --enable=warning,style --inconclusive --std=c++20 --quiet --check-level=exhaustive || {
        echo "cppcheck found issues." >&2
        exit 3
      }
    fi
  else
    echo "cppcheck not found; skipping static analysis."
  fi
fi

echo "Format check complete."
exit 0
