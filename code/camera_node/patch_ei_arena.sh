#!/bin/bash
# Fix Edge Impulse compiled tensor arena: metadata needs ~256403 B; stock library used ~246720 B.
# Sets 300000 B in PSRAM (.ext_ram.bss) for XIAO ESP32S3.
set -euo pipefail

LIB="${1:-$HOME/Documents/Arduino/libraries/cat-litter-box_inferencing}"
CPP="$LIB/src/tflite-model/tflite_learn_994100_4_compiled.cpp"

if [[ ! -f "$CPP" ]]; then
  echo "Not found: $CPP"
  echo "Usage: $0 [path/to/cat-litter-box_inferencing]"
  exit 1
fi

cp "$CPP" "$CPP.bak"

python3 - "$CPP" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
text = re.sub(
    r"constexpr int kTensorArenaSize = \d+;",
    "constexpr int kTensorArenaSize = 300000;",
    text,
)
old = """#if defined (EI_TENSOR_ARENA_LOCATION)
uint8_t tensor_arena[kTensorArenaSize] ALIGN(16) DEFINE_SECTION(STRINGIZE_VALUE_OF(EI_TENSOR_ARENA_LOCATION));"""
new = """#if defined (EI_TENSOR_ARENA_LOCATION)
static uint8_t tensor_arena[300000] __attribute__((section(".ext_ram.bss")));"""
if old not in text and new not in text:
    # already patched or different EI version
    if "tensor_arena[300000]" not in text:
        print("WARN: could not find stock arena declaration; edit manually.", file=sys.stderr)
else:
    text = text.replace(old, new)
open(path, "w").write(text)
print("Patched:", path)
PY

echo "Done. In Arduino IDE: Sketch -> Verify (full rebuild), then upload camera_node."
echo "Backup: $CPP.bak"
