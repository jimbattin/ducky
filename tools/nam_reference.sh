#!/usr/bin/env bash
# Render NAM reference outputs with NeuralAmpModelerCore's tools/render at a pinned commit
# (docs/NAM_FORMAT.md "Reference renders").
#
#   tests/fixtures/nam/A2.input.ref.wav   committed: the MIT A2.nam on the upstream input.wav
#   renders/ref/<model>.<stimulus>.ref.wav  local: models/*.nam and A2.nam on input.wav and guitar_di.wav
#
# Every render uses --slim 0.25, which selects the A2-Lite submodel.
#
# Usage: tools/nam_reference.sh
set -euo pipefail
cd "$(dirname "$0")/.."

NAMCORE_COMMIT=d3189ed482ded880c128bf312941266c4fa5ceec
NAMCORE_URL=https://github.com/sdatkinson/NeuralAmpModelerCore
CORE=build/namcore
RENDER=build/namcore-render

[[ -d $CORE/.git ]] || git clone -q "$NAMCORE_URL" "$CORE"
git -C "$CORE" checkout -q "$NAMCORE_COMMIT"
git -C "$CORE" submodule update --init --recursive -q

# Plain g++ rather than upstream CMake (tools/CMakeLists.txt's source_group fails under CMake 4.5).
# -O2 instead of upstream Release's -Ofast keeps the references strict IEEE.
if [[ ! -x $RENDER ]]; then
    echo "-- building $RENDER"
    g++ -std=c++20 -O2 -DNAM_ENABLE_A2_FAST \
        -I"$CORE" -I"$CORE/Dependencies/eigen" -I"$CORE/Dependencies/nlohmann" \
        -I"$CORE/Dependencies/AudioDSPTools/dsp" \
        "$CORE/tools/render.cpp" "$CORE"/NAM/*.cpp "$CORE"/NAM/*/*.cpp \
        "$CORE/Dependencies/AudioDSPTools/dsp/wav.cpp" -o "$RENDER"
fi

if [[ -f ../spx90/guitar.wav ]]; then
    uv run tools/make_stimuli.py
else
    echo "-- skipped guitar_di (no ../spx90/guitar.wav)"
fi

render() {
    "$RENDER" --slim 0.25 "$1" "$2" "$3"
    echo "$3"
}

render tests/fixtures/nam/A2.nam tests/fixtures/nam/input.wav tests/fixtures/nam/A2.input.ref.wav

mkdir -p renders/ref
stimuli=(tests/fixtures/nam/input.wav)
[[ -f renders/stimuli/guitar_di.wav ]] && stimuli+=(renders/stimuli/guitar_di.wav)
shopt -s nullglob
for model in models/*.nam tests/fixtures/nam/A2.nam; do
    for stim in "${stimuli[@]}"; do
        render "$model" "$stim" "renders/ref/$(basename "$model" .nam).$(basename "$stim" .wav).ref.wav"
    done
done
