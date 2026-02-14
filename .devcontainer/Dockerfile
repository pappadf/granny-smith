# .devcontainer/Dockerfile
# Purpose: Build a general-purpose dev image with Emscripten 4.0.10 + C/C++ + Node LTS

FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# ---- ARGs / Versions ----
ARG DEBIAN_FRONTEND=noninteractive
ARG EMSDK_VERSION=4.0.10

# ---- Base packages (includes system Node.js LTS from Ubuntu repos) ----
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    ca-certificates \
    curl \
    git \
    xz-utils \
    nodejs \
    npm \
 && rm -rf /var/lib/apt/lists/*

# Verify node/npm available
RUN node -v && npm -v

# ---- Emscripten (emsdk 4.0.10) ----
ENV EMSDK=/opt/emsdk
RUN git clone --depth=1 https://github.com/emscripten-core/emsdk ${EMSDK} \
 && bash -lc "cd ${EMSDK} && ./emsdk install ${EMSDK_VERSION} && ./emsdk activate ${EMSDK_VERSION}" \
 && rm -rf ${EMSDK}/.git \
 && printf 'source /opt/emsdk/emsdk_env.sh >/dev/null 2>&1 || true\n' > /etc/profile.d/emsdk.sh

# Persist emscripten toolchain PATH for non-login shells (CI container steps)
ENV PATH="/opt/emsdk:/opt/emsdk/upstream/emscripten:/opt/emsdk/node/$(ls /opt/emsdk/node | head -n 1)/bin:${PATH}"

# Ensure non-login shells (like Docker RUN) can access emcc by sourcing env explicitly
SHELL ["/bin/bash", "-lc"]

# ---- Smoke tests (Node + Emscripten) ----
RUN source /etc/profile.d/emsdk.sh && node -v && npm -v && emcc -v | head -n 5

# ---- Playwright (optional: deps installed at runtime) ----
# We intentionally do NOT install browsers here to keep the image lean.
# They’ll be installed via postCreateCommand/CI step: `npx playwright install --with-deps`

# (Removed global npm update to preserve determinism)

