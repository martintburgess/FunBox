#!/usr/bin/env bash
# Runs once after the container is created.
set -uo pipefail

CLAUDE_DIR="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"

# The named volume mounts as root-owned; hand it to our user so Claude Code
# can write credentials/history into it.
sudo chown -R "$(id -u):$(id -g)" "$CLAUDE_DIR" 2>/dev/null || true

# Build the Daisy libraries so any pedal under software/ is ready to compile.
# Non-fatal: a failure here shouldn't block the container from coming up.
echo ">> Building libDaisy ..."
make -C libDaisy   || echo "!! libDaisy build failed - run 'make -C libDaisy' to debug"
echo ">> Building DaisySP ..."
make -C DaisySP    || echo "!! DaisySP build failed - run 'make -C DaisySP' to debug"

cat <<'EOF'

============================================================
FunBox dev container ready.

Build a pedal:      cd software/Venus && make
  -> produces:      software/Venus/build/venus.bin

Flashing: Docker on macOS can't see the Daisy's USB, so flash
the .bin from your HOST via https://flash.daisy.audio
(Daisy in DFU mode: hold BOOT, tap RESET, release BOOT).
The build/ folder is on your host filesystem, so the .bin is
right there in software/Venus/build/.
============================================================
EOF
