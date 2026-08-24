#!/bin/bash
set -e

echo "Building PipeWire TTS Streaming Example..."

# Assuming libmoonshine has been built or downloaded to ../../c++/moonshine-voice
# as described in the C++ examples README.
MOONSHINE_INC="../../c++/moonshine-voice/include"
MOONSHINE_LIB="../../c++/moonshine-voice/lib"

if [ ! -d "$MOONSHINE_INC" ]; then
    echo "Warning: $MOONSHINE_INC not found."
    echo "Please download the Moonshine library first (e.g., using examples/c++/download-library.sh) or adjust the paths if building from source."
fi

g++ moonshine-tts-streaming.cpp \
    -std=c++20 \
    -O3 \
    -I"${MOONSHINE_INC}" \
    -L"${MOONSHINE_LIB}" \
    -lmoonshine \
    -Wl,-rpath,'$ORIGIN/../../c++/moonshine-voice/lib' \
    $(pkg-config --cflags --libs libpipewire-0.3) \
    -o moonshine-tts-streaming

echo "Build successful! Created executable: moonshine-tts-streaming"
