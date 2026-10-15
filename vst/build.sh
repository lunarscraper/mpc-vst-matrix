#!/usr/bin/env bash
# Build Matrix as a VST2 insert effect for the MPC OS plugin host (armhf).
#   vst/build/matrix.so             -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN ... category="Effect"> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
# matrix_vst.cpp + the engine matrix_core.h + the grid page matrix_web.h (page/index.html) on the
# shared hub forcehub.h, linked against the device's libasound (ALSA seq port, as in DrumGen FX).
# vst.json/module.json only feed tools/gen_vst.py for params.h + the skin; captions.py then redraws
# the labels in Titillium Web at the knob names' size. Offline test: test.sh.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h (PLUG_EFFECT), skin, pluginlist-entry.xml (needs Pillow), then the captions
SKIN="build/skin/lunarscraper - VST - Matrix/Plugin Skins"
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
  python3 captions.py layout.conf "$SKIN" fonts/TitilliumWeb-SemiBold.ttf
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json && PYTHONPATH=/tmp/p python3 captions.py layout.conf \"$SKIN\" fonts/TitilliumWeb-SemiBold.ttf"
fi
grep -q "PLUG_EFFECT" build/params.h || { echo "params.h has no PLUG_EFFECT"; exit 1; }
cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper
python3 gen_page.py                    # page/index.html (grid) -> matrix_page.h

# 3. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39)
docker run --rm --platform linux/arm/v7 -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter -Ibuild -I. \
      -o build/matrix.so -shared matrix_vst.cpp \
      -static-libstdc++ -static-libgcc -lasound -lpthread -lm -Wl,--no-undefined
  strip build/matrix.so
  echo "-- exported --"; readelf --dyn-syms -W build/matrix.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/matrix.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/matrix.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/matrix.so
