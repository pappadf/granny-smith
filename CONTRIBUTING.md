# Contributing to Granny Smith

Thank you for your interest in contributing to Granny Smith! We welcome contributions from the community to help improve and extend this Macintosh emulator.

## Getting Started

1. **Fork** the repository on GitHub
2. **Clone** your fork with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/YOUR-USERNAME/granny-smith.git
   cd granny-smith
   ```
3. **Create a branch** for your changes:
   ```bash
   git checkout -b feature/your-feature-name
   ```

## Development Setup

### Using the Devcontainer (Recommended)

The easiest way to get started is with the preconfigured devcontainer, which includes all prerequisites:

- Open the repository in VS Code or GitHub Codespaces
- The devcontainer image has Emscripten 6.0.7, Node.js 22.x, Playwright, and all build tools preinstalled

### Manual Setup

1. Install Emscripten 6.0.7:
   ```bash
   git clone https://github.com/emscripten-core/emsdk
   cd emsdk && ./emsdk install 6.0.7 && ./emsdk activate 6.0.7
   source ./emsdk_env.sh
   ```
2. Install Node.js 20.9+ and npm (the frontend's `engines` floor; the dev container ships 22), and m68k binutils (`binutils-m68k-linux-gnu`), which assemble the generic declaration-ROM fragments
3. Install Playwright (for end-to-end tests):
   ```bash
   cd tests/e2e && npm ci
   npx playwright install --with-deps chromium
   ```

**Required tools:** `emcc` (6.0.7), `make`, `node` (20.9+), `python3`, `git`, `binutils-m68k-linux-gnu`. The dev container (`.devcontainer/Dockerfile`) has all of them.

### Building

```bash
make clean && make     # WASM release build (~30 sec)
make debug             # WASM debug build
make headless          # Native headless CLI (no Emscripten required)
```

Every build uses `-std=gnu11 -Wall -Wextra`, and CI adds `WERROR=1`, which
makes any warning an error: build with `make WERROR=1` (and
`make -f Makefile.headless WERROR=1`) before pushing.

Output goes to the `build/` directory.

### Running Locally

```bash
make run               # Build and start HTTP server on :8080
```

Or use `scripts/dev_server.py` directly. The dev server sets the required COOP/COEP headers for SharedArrayBuffer support.

## Testing

```bash
make -j$(nproc) -C tests/unit run      # Unit tests
make integration-test TIER=unit        # The fast integration tier
make integration-test -j$(nproc)       # All integration tiers (long)
make test                              # Unit + integration tests

# End-to-end tests (requires Playwright + test data)
make ui2-e2e
```

What each tier covers, what it needs and how long it takes: [docs/guide/TESTING.md](docs/guide/TESTING.md).

Please ensure all existing tests pass before submitting a PR. Add tests for new functionality where practical.

## Types of Contributions

### Bug Reports
- Use the GitHub issue tracker
- Include steps to reproduce
- Mention which browser/OS you tested on
- Include terminal output or screenshots if relevant

### Feature Requests
- Open an issue to discuss the feature first
- Explain the use case and rationale

### Documentation Improvements
- Fix typos, improve clarity
- Add examples or expand technical documentation in `docs/`

### Code Contributions
- Follow the project's coding style (see below)
- Add tests for new functionality
- Keep changes focused — one feature or fix per PR

## Code Style

Please follow the conventions in [docs/guide/STYLE_GUIDE.md](docs/guide/STYLE_GUIDE.md). Key points:

- C11 (GNU dialect: the WASM build's `EM_ASM` needs it)
- `snake_case` for all identifiers
- Use `//` for short, inline comments
- Each function and structure gets a one-line comment above it describing its purpose
- For each significant statement or calculation, add a concise one-line comment
- Keep changes small and focused on the problem
- Do not change or remove existing comments unnecessarily
- Put prototypes in headers when needed
- Add `// SPDX-License-Identifier: MIT` at the top of new source files

### Formatting

The project uses `clang-format-18` to enforce consistent code style. The devcontainer includes pre-commit hooks that automatically check formatting before each commit.

- **In the devcontainer**: Hooks are automatically installed during setup
- **Manual setup**: Run `pip install pre-commit && pre-commit install` in the repository root
- **Check formatting**: Run `pre-commit run --all-files` to check all files

If the CI formatting check fails, the pre-commit hooks will catch it locally before you push.

## Repository Structure

| Directory | Contents |
|-----------|----------|
| `src/core/` | Platform-agnostic emulator (CPU, memory, peripherals, etc.) |
| `src/platform/` | Platform-specific code (wasm/, headless/) |
| `app/web2/` | Browser frontend (Svelte 5 + Vite + TS) |
| `docs/` | Architecture and hardware documentation |
| `tests/` | Unit, integration, and end-to-end tests |
| `scripts/` | Build and helper scripts |
| `third-party/` | CPU test corpora (git submodules: single-step-tests, powerpc-test) |

For a deeper overview, see [AGENTS.md](AGENTS.md).

## Submission Process

1. **Commit** your changes with clear, descriptive messages
2. **Push** to your fork
3. **Open a Pull Request** with:
   - A clear description of the changes
   - Reference to any related issues
   - Confirmation that tests pass

## Review Process

- All contributions will be reviewed before merging
- Reviewers may request changes or improvements
- Please be patient and responsive to feedback

## License

By contributing to Granny Smith, you agree that your contributions will be licensed under the same [MIT License](LICENSE) that covers the project.
