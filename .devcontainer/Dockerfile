FROM mcr.microsoft.com/devcontainers/base:bookworm

# --- Embedded ARM toolchain + build/flash tooling for Daisy firmware ---
# apt packages auto-resolve to the container arch (linux/arm64 on Apple Silicon,
# linux/amd64 on Intel), so this image builds on any host.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
 && apt-get install -y --no-install-recommends \
      gcc-arm-none-eabi \
      binutils-arm-none-eabi \
      libnewlib-arm-none-eabi \
      libstdc++-arm-none-eabi-newlib \
      build-essential \
      make \
      dfu-util \
      curl \
      ca-certificates \
      git \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# --- Claude Code (native standalone install, no Node.js required) ---
# Installed for the non-root `vscode` user into ~/.local/bin.
USER vscode
RUN curl -fsSL https://claude.ai/install.sh | bash
USER root

# Put the native Claude binary on PATH for all container processes.
ENV PATH="/home/vscode/.local/bin:${PATH}"
