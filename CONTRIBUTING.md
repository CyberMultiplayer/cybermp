# Contributing to cybermp

Thank you for your interest in contributing! This document explains how to get your changes merged smoothly.

---

## Table of contents

1. [Code of conduct](#code-of-conduct)
2. [How to report a bug](#how-to-report-a-bug)
3. [How to propose a feature](#how-to-propose-a-feature)
4. [Development workflow](#development-workflow)
5. [Code style](#code-style)
6. [Commit conventions](#commit-conventions)
7. [Pull request checklist](#pull-request-checklist)

---

## Code of conduct

Be respectful and constructive. Harassment or hostile behavior of any kind will not be tolerated.

---

## How to report a bug

1. Search existing [issues](https://github.com/yutho-o/cybermp/issues) first.
2. If your issue is new, open one and include:
   - Cyberpunk 2077 version
   - RED4ext version
   - cybermp version or commit hash
   - Steps to reproduce
   - Expected vs. actual behavior
   - Relevant log output (RED4ext log at `<game>/red4ext/logs/cybermp.log`)

---

## How to propose a feature

Open an issue describing the feature and its motivation **before** writing code. This avoids duplicated effort and makes sure the direction fits the project.

---

## Development workflow

### 1. Fork and clone

```bash
git clone --recurse-submodules https://github.com/<your-fork>/cybermp.git
cd cybermp
```

### 2. Create a branch

Branch off `main` with a short, descriptive name:

```bash
git checkout -b feat/player-sync
```

### 3. Set up your build

See [README.md](README.md) for full instructions. The short version:

```bash
cmake -S . -B build -A x64 [-DCYBERMP_GAME_DIR="D:/Cyberpunk 2077"]
cmake --build build --config Release
```

### 4. Make your changes

Keep commits focused — one logical change per commit.

### 5. Verify the CI checklist locally

Before pushing, confirm the build still passes and that the DLL exports are intact:

```powershell
cmake --build build --config Release
```

The GitHub Actions workflow (`build.yml`) will run the same checks automatically on your pull request.

### 6. Open a pull request

Push your branch to your fork and open a pull request against `main`. Fill in the PR description with a summary of what changed and why.

---

## Code style

The project follows the style enforced by `.clang-format` at the repository root. Before committing, format your changes:

```bash
clang-format -i src/client/*.cpp src/client/*.hpp
```

Additional guidelines:

- **C++20** — use modern features (`if constexpr`, ranges, concepts, …) where they improve clarity.
- **Headers** — use `#pragma once`.
- **Naming** — follow the conventions already in the codebase: `PascalCase` for types, `camelCase` with a leading `a` prefix for parameters (e.g., `aHandle`), `m_` prefix for member variables, `s_` for statics.
- **Includes** — keep includes minimal and grouped: standard library, then SDK/vendor, then local headers.
- **Comments** — write comments that explain *why*, not *what*. Match the concise style already present in the source files.
- **No warnings** — the project compiles with `/W4 /permissive-`. New code must not introduce warnings.

---

## Commit conventions

Use the following prefix format inspired by [Conventional Commits](https://www.conventionalcommits.org/):

| Prefix | When to use |
|--------|-------------|
| `feat:` | A new feature |
| `fix:` | A bug fix |
| `refactor:` | Code restructuring without behavior change |
| `docs:` | Documentation only |
| `ci:` | CI / workflow changes |
| `chore:` | Maintenance (deps, tooling, …) |

Keep the subject line under 72 characters and in the imperative mood:

```
feat: expose player position over the network socket
```

---

## Pull request checklist

Before requesting a review, confirm:

- [ ] `cmake --build build --config Release` completes without errors or new warnings.
- [ ] Code is formatted with `clang-format`.
- [ ] Commit messages follow the conventions above.
- [ ] The PR description explains *what* changed and *why*.
- [ ] No debug-only code, temporary `#if 0` blocks, or commented-out leftovers are included.
