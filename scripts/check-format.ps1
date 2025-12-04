<#
SPDX-License-Identifier: MIT
Copyright (c) 2025 LinuxUDPShardedEcho Contributors
#>

param(
  [switch]$Fix,
  [switch]$All,
  [Alias('v')][switch]$VerboseMode
)

Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..")
Set-Location $Root

# Native PowerShell implementation
# Build file lists with strict extension filtering
$all_files = @()
if ($All) {
  $all_files = & git ls-files 2>$null | ForEach-Object { $_.ToString().Trim() }
} else {
  $staged = & git diff --cached --name-only --diff-filter=ACM 2>$null | ForEach-Object { $_.ToString().Trim() }
  $modified = & git diff --name-only --diff-filter=ACM 2>$null | ForEach-Object { $_.ToString().Trim() }
  $all_files = ($staged + $modified) | Where-Object { $_ -ne '' } | Sort-Object -Unique
}

# Exclude generated expression outputs (created by scripts/process_expressions.ps1)
$all_files = $all_files | Where-Object {
  $p = $_ -replace '\\','/'
  -not ($p -match '^(tests/regression_tests/expression_tree_|tests/regression_tests/expression_bdd_)')
}

if (-not $all_files -or (@($all_files)).Count -eq 0) {
  if ($All) { Write-Output "No files found in repository." } else { Write-Output "No changes to check - OK" }
  exit 0
}

if ($All -or $Fix -or $VerboseMode) {
  Write-Output "Files considered for checks:"
  $all_files | ForEach-Object { Write-Output "  $_" }
}

# Extensions considered C/C++ source files
$srcExt = @('c','cpp','cc','cxx','h','hpp','hh','hxx')

# Extensions that should have SPDX headers
$spdxExt = @('c','cpp','cc','cxx','h','hpp','hh','hxx','cmake','sh','ps1','md','markdown','yml','yaml','txt')

$SRC_FILES = $all_files | Where-Object { $e = ($_ -split '\.')[-1].ToLower(); $srcExt -contains $e }
$SPDX_CANDIDATES = $all_files | Where-Object { $e = ($_ -split '\.')[-1].ToLower(); $spdxExt -contains $e }

if ((@($SPDX_CANDIDATES)).Count -gt 0) {
  if ($All -or $Fix -or $VerboseMode) { Write-Output "Checking SPDX headers for these files:"; $SPDX_CANDIDATES | ForEach-Object { Write-Output "  $_" } }
  $missing = @()
  foreach ($f in $SPDX_CANDIDATES) {
    if (-not (Test-Path $f)) { continue }
    $head = Get-Content -Path $f -TotalCount 20 -ErrorAction SilentlyContinue
    if (-not ($head -match 'SPDX-License-Identifier')) {
      $missing += $f
    }
  }
  if ((@($missing)).Count -gt 0) {
    Write-Error "The following files are missing an SPDX-License-Identifier in the first 20 lines:"
    $missing | ForEach-Object { Write-Error "  $_" }
    exit 4
  }
} else {
  Write-Output "No SPDX-eligible files found in selection."
}

# Run clang-format only on C/C++ files
if ((@($SRC_FILES)).Count -gt 0) {
    Write-Output "Running clang-format on C/C++ files..."
  foreach ($f in $SRC_FILES) {
    if ($Fix) { & clang-format -style=file -i $f } else { & clang-format -style=file -n --Werror $f }
    if ($LASTEXITCODE -ne 0) {
      Write-Error "clang-format reported issues on $f"
      exit 2
    }
  }
} else {
  Write-Output "No C/C++ source files selected for formatting/static analysis."
}

# Run cppcheck on C/C++ files only if present
if ((@($SRC_FILES)).Count -gt 0) {
  if (Get-Command cppcheck -ErrorAction SilentlyContinue) {
    Write-Output "Running cppcheck on C/C++ files..."
    $paths = @($SRC_FILES)
    & cppcheck --enable=warning,style --inconclusive --std=c++20 --quiet --check-level=exhaustive $paths
    if ($LASTEXITCODE -ne 0) {
      Write-Error "cppcheck found issues."
      exit 3
    }
  } else {
    Write-Output "cppcheck not found; skipping static analysis."
  }
}

Write-Output "Format check complete."
exit 0
