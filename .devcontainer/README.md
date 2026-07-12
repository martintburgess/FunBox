# FunBox dev container

A reproducible environment for building Funbox / Daisy Seed firmware, so you
don't have to install the ARM toolchain on your host.

## What's inside
- **ARM embedded toolchain** (`arm-none-eabi-gcc/g++`, newlib, libstdc++) + `make`
- **dfu-util** (for reference; USB flashing is done from the host — see below)
- **Claude Code** (native install, no Node.js) on `PATH` at `~/.local/bin/claude`

## Getting started
Open the repo in a devcontainer-aware editor ("Reopen in Container") or with the
CLI:

```bash
devcontainer up --workspace-folder .
```

On first create it builds `libDaisy` and `DaisySP` (a few minutes). Then:

```bash
cd software/Venus && make      # -> software/Venus/build/venus.bin
```

## Flashing (done on the HOST, not in the container)
Docker Desktop on macOS can't pass the Daisy's USB into a Linux container, so the
container **builds** the firmware and you **flash** from your Mac:

1. `software/<pedal>/build/<pedal>.bin` is on your host filesystem (the workspace
   is bind-mounted), so it's already there.
2. Put the Daisy in DFU mode: hold **BOOT**, tap **RESET**, release **BOOT**.
3. Open <https://flash.daisy.audio> and upload the `.bin`.
   - Flash-based pedals (Venus, Earth, Pluto, Uranus): flash directly.
   - Bootloader pedals (Mercury, Mars): flash the bootloader first, then the app.

## Claude Code login persistence
`CLAUDE_CONFIG_DIR` is set to `~/.claude`, which is a **named Docker volume**
(`funbox-claude`). Your login and history persist there across container
rebuilds. To reset, remove the volume: `docker volume rm funbox-claude`.

## Notes / tweaks
- The toolchain comes from Debian apt (`gcc-arm-none-eabi`, ~12.2). If a pedal
  ever needs a newer toolchain than Debian ships, swap the apt install in the
  `Dockerfile` for the official ARM GNU toolchain tarball.
- Rebuild the image after editing the `Dockerfile`: "Rebuild Container".
