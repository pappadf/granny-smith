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
- The devcontainer image has Emscripten 4.0.10, Node.js 18.x, Playwright, and all build tools preinstalled

### Manual Setup

1. Install Emscripten 4.0.10:
   ```bash
   git clone https://github.com/emscripten-core/emsdk
   cd emsdk && ./emsdk install 4.0.10 && ./emsdk activate 4.0.10
   source ./emsdk_env.sh
   ```
2. Install Node.js 18+ and npm
3. Install Playwright (for end-to-end tests):
   ```bash
   cd tests/e2e && npm ci
   npx playwright install --with-deps chromium
   ```

**Required tools:** `emcc` (4.0.10), `make`, `node` (18+), `python3`, `git`

### Building

```bash
make clean && make     # WASM release build (~30 sec)
make debug             # WASM debug build
make headless          # Native headless CLI (no Emscripten required)
```

Output goes to the `build/` directory.

### Running Locally

```bash
make run               # Build and start HTTP server on :8080
```

Or use `scripts/dev_server.py` directly. The dev server sets the required COOP/COEP headers for SharedArrayBuffer support.

## Testing

```bash
make integration-test                  # Headless integration tests (~1–2 min)
make -C tests/unit run                 # CPU unit tests (~1–5 min)
make test                              # Unit + integration tests

# End-to-end tests (requires Playwright + test data)
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.config.ts
```

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

Please follow the conventions in [docs/STYLE_GUIDE.md](docs/STYLE_GUIDE.md). Key points:

- C99 standard
- `snake_case` for all identifiers
- Use `//` for short, inline comments
- Each function and structure gets a one-line comment above it describing its purpose
- For each significant statement or calculation, add a concise one-line comment
- Keep changes small and focused on the problem
- Do not change or remove existing comments unnecessarily
- Put prototypes in headers when needed
- Add `// SPDX-License-Identifier: MIT` at the top of new source files

## Repository Structure

| Directory | Contents |
|-----------|----------|
| `src/core/` | Platform-agnostic emulator (CPU, memory, peripherals, etc.) |
| `src/platform/` | Platform-specific code (wasm/, headless/) |
| `app/web/` | Browser frontend (HTML, JS, CSS) |
| `docs/` | Architecture and hardware documentation |
| `tests/` | Unit, integration, and end-to-end tests |
| `scripts/` | Build and helper scripts |
| `third-party/` | External libraries (peeler, single-step-tests) |

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
