#!/bin/bash
set -e

echo "Building PipeWire TTS Streaming Example..."

# Prioritize the locally built Moonshine library (from core/build) if compiling from source.
# Fallback to the pre-compiled downloaded library (from examples/c++/moonshine-voice) for end-users.

if [ -f "../../../core/build/libmoonshine.so" ]; then
    echo "Found locally built libmoonshine.so in core/build. Using local source version."
    MOONSHINE_INC="../../../core"
    MOONSHINE_LIB="../../../core/build"
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ]; then
        ONNX_LIB="../../../core/third-party/onnxruntime/lib/linux/x86_64"
    else
        ONNX_LIB="../../../core/third-party/onnxruntime/lib/linux/aarch64"
    fi
else
    echo "Local build not found. Falling back to pre-compiled downloaded library..."
    MOONSHINE_INC="../../c++/moonshine-voice/include"
    MOONSHINE_LIB="../../c++/moonshine-voice/lib"
    ONNX_LIB="../../c++/moonshine-voice/lib"
    if [ ! -d "$MOONSHINE_INC" ]; then
        echo "Warning: $MOONSHINE_INC not found."
        echo "Please download the Moonshine library first (e.g., using examples/c++/download-library.sh) or build the core library from source."
        exit 1
    fi
fi

g++ moonshine-tts-streaming.cpp \
    -std=c++20 \
    -O3 \
    -I"${MOONSHINE_INC}" \
    -L"${MOONSHINE_LIB}" \
    -L"${ONNX_LIB}" \
    -lmoonshine \
    -l:libonnxruntime.so.1 \
    -Wl,-rpath,'$ORIGIN/'"${MOONSHINE_LIB}" \
    -Wl,-rpath,'$ORIGIN/'"${ONNX_LIB}" \
    $(pkg-config --cflags --libs libpipewire-0.3) \
    -o moonshine-tts-streaming

echo "Build successful! Created executable: moonshine-tts-streaming"
