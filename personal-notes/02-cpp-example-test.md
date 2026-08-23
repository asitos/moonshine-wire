following the readme.md present in examples/c++/, im gonna test it out

a transcriber binary was built which just outputs some lines? im gonna go through the text-to-speech.cpp and transcriber.cpp and also use an llm to help me understand the code

update: following this readme compiles the binary, but results in an error:
```
Thread 139866139486976:moonshine-c-api.cpp:1360:moonshine_create_tts_synthesizer_from_files(): Failed to create TTS synthesizer: FileInformation::load: cannot open ../../core/moonshine-tts/data/kokoro/model.ort

Moonshine error: Unknown error
```

update 2: it works, i removed the local folder and started again with a clean build, the tts works now, that took an hour
