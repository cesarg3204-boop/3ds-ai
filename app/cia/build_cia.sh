#!/bin/sh
# Builds SpikeAI.cia from SpikeAI.elf (run `make` first).
# Needs makerom from https://github.com/3DSGuy/Project_CTR (MAKEROM=path/to/makerom).
cd "$(dirname "$0")/.." || exit 1
"${MAKEROM:-makerom}" -f cia -o SpikeAI.cia -elf SpikeAI.elf -rsf cia/app.rsf -icon SpikeAI.smdh -target t -exefslogo
