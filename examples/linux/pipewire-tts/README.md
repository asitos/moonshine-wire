# Moonshine PipeWire TTS Streaming Example

This example demonstrates an ultra-low-latency real-time Text-to-Speech (TTS) streaming daemon for Linux, built on top of Moonshine and PipeWire.

It accepts incremental text over a Unix domain socket, processes it in chunks, generates IPA phonemes dynamically, and outputs lock-free 32-bit float PCM audio directly to a PipeWire real-time callback. It also features instant audio preemption (speech cancellation).

## Prerequisites

You need the `libpipewire-0.3-dev` package to build this example:

```bash
sudo apt update
sudo apt install libpipewire-0.3-dev pipewire-audio socat
```

You must also have the Moonshine C++ library available. If you haven't already, you can download the pre-compiled library by running the download script from the `examples/c++/` directory:

```bash
cd ../../c++
./download-library.sh
cd ../linux/pipewire-tts
```

## Building

Run the provided build script. It uses `pkg-config` to locate PipeWire and assumes the Moonshine headers/libraries are available in the adjacent `c++` example folder.

```bash
./build.sh
```

## Running the Daemon

Start the daemon and point it to the downloaded Moonshine assets:

```bash
./moonshine-tts-streaming \
    --model-root ../../c++/moonshine-voice/model \
    --lang en_us \
    --voice kokoro_af_heart
```

This will create two Unix domain sockets in `/tmp`:
- `/tmp/moonshine-tts.sock` (Input: send text here)
- `/tmp/moonshine-phonemes.sock` (Output: broadcasts IPA phonemes)

## Testing the Stream

In a separate terminal, connect to the socket and send text:

```bash
echo "Hello from Moonshine, streamed dynamically over PipeWire." | socat - UNIX-CONNECT:/tmp/moonshine-tts.sock
```

### Testing Instant Preemption (Cancellation)

If you send the ASCII Cancel byte (`\x18`), the daemon will instantly flush all pending audio, text queues, and ringbuffers without waiting for the current sentence to finish. This is crucial for interactive robotics.

```bash
echo -n "This is a very long sentence that will be interrupted immediately." | socat - UNIX-CONNECT:/tmp/moonshine-tts.sock
sleep 1
echo -n -e '\x18' | socat - UNIX-CONNECT:/tmp/moonshine-tts.sock
```

### Monitoring Phonemes

To watch the phonemes being generated synchronously as text is processed:

```bash
socat - UNIX-CONNECT:/tmp/moonshine-phonemes.sock
```
