<!-- SPDX-License-Identifier: MIT
  Copyright (c) 2025 LinuxUDPShardedEcho contributors -->
# Contributing to LinuxUDPShardedEcho

Thanks for helping improve `LinuxUDPShardedEcho` — a small C++ echo server / client demo used to illustrate scalable I/O patterns and build/test workflows on Windows (MSVC + CMake).

Please read the guidelines below before opening issues or pull requests.

---

## Quick Start

1. Fork the repository on GitHub and clone your fork locally:

```pwsh
git clone https://github.com/<your-username>/LinuxUDPShardedEcho.git
cd LinuxUDPShardedEcho
```

2. Create a feature branch:

```pwsh
git checkout -b feature/my-change
```

3. Build (recommended):

```pwsh
mkdir build
cd build
cmake -S .. -B . -A x64
cmake --build . --config Release
```

Or open `build/LinuxUDPShardedEcho.sln` in Visual Studio and build from the IDE.

---

## What to Change

- Fixes and small improvements: tests, build fixes, documentation, small refactors
- New features: limited in scope to the demo (e.g., improved logging, more test coverage)
- Do not add large third-party libraries; prefer lightweight dependencies and document them in the README.

## Coding Guidelines

- Language: C++17/C++20 depending on the target (project is configured for MSVC; keep compatibility in mind).
- Follow existing style in the codebase. If you change formatting, include `.clang-format` adjustments in a separate commit.
- Keep changes small and focused; one logical change per PR.

## Tests

- Tests are located under `src/` alongside client/server components (if present). Use `ctest` from the `build` directory to run tests after building.
- Add tests for bug fixes and new features where practical.

## Commit Messages

Use clear, short commit messages. Examples:

```pwsh
feat: add graceful shutdown to server
fix: correct socket cleanup on client disconnect
docs: update CONTRIBUTING.md for this repo
```

## Pull Request Checklist

- [ ] Branch off `main` (this repo's development branch)
- [ ] Include a clear description and motivation
- [ ] Add or update tests for behavior changes
- [ ] Ensure the project builds on Windows with MSVC (x64)
- [ ] Run formatting checks (`clang-format`) and static checks if available

## CI and Checks

- This repository uses CMake and has a `build/` solution generated; CI may be configured by the project maintainer. If you add GitHub Actions workflows, keep them minimal and Windows-focused unless you add cross-platform support.

## Reporting Issues

- Open an issue for bugs or feature requests. Include steps to reproduce, expected vs actual behavior, and relevant logs or stack traces.

## Code of Conduct

Please be respectful and constructive. We follow the Contributor Covenant: https://www.contributor-covenant.org/.

---

Happy hacking! 🚀
