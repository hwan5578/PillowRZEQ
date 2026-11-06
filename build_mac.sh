#!/bin/bash
# Pillow RZ EQ - macOS build (universal VST3 + AU) and install to ~/Library/Audio/Plug-Ins
set -euo pipefail
cd "$(dirname "$0")"

command -v cmake >/dev/null 2>&1 || { echo "CMake not found. Install once:  brew install cmake"; exit 1; }
command -v git   >/dev/null 2>&1 || { echo "Git not found. Install once:  xcode-select --install"; exit 1; }
xcode-select -p  >/dev/null 2>&1 || { echo "Xcode command line tools not found:  xcode-select --install"; exit 1; }

echo "[1/3] Configure (first run downloads JUCE)"
cmake -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

echo "[2/3] Build VST3 + AU"
cmake --build build --config Release --target PillowRZ_VST3 PillowRZ_AU --parallel

OUT="build/PillowRZ_artefacts/Release"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"

echo "[3/3] Install"
mkdir -p "$VST3_DIR" "$AU_DIR"
rm -rf "$VST3_DIR/Pillow RZ EQ.vst3" "$AU_DIR/Pillow RZ EQ.component"
cp -R "$OUT/VST3/Pillow RZ EQ.vst3" "$VST3_DIR/"
cp -R "$OUT/AU/Pillow RZ EQ.component" "$AU_DIR/"
codesign --force --deep --sign - "$VST3_DIR/Pillow RZ EQ.vst3" "$AU_DIR/Pillow RZ EQ.component" >/dev/null 2>&1 || true

echo
echo "Done. Rescan plug-ins in Cubase (Studio > VST Plug-in Manager)."
