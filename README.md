# MetaVoice

GoldSrc **voice capture** plugin for [MetaHookSv](https://github.com/hzqst/MetaHookSv).

[MetaAudio](https://github.com/darwin128k/MetaAudio) replaces how the game **plays** world sound (OpenAL).  
**MetaVoice** is the other half: how the game **records** your microphone and what you can change before Speex goes to the server.

It does **not** go through the Windows recording mixer for the settings we own. Capture stays on the engine WaveIn path (Bluetooth-safe: the mic opens only on `+voicerecord` / Test Microphone, then closes).

## What it does today

- Forces the native Speex + WaveIn path (RevEmu/Steam voice stub is skipped).
- Opens capture only when you talk or test the mic; closes it after, so Bluetooth can return to A2DP.
- Rebuilds codec + recorder after a level change (stock GoldSrc never does).
- Registers software-gain cvars for the Options UI (values persist; DSP gain in the capture buffer is the next step).

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `mv_gain` | `1.0` | Transmit level, 0–1. Replaces Windows “Voice transmit volume”. |
| `mv_boost` | `0` | Extra gain on/off. Replaces Windows “Boost microphone gain”. |

Do not load **VoiceFix.dll** and **MetaVoice.dll** together.

## Build

Needs MetaHookSv headers. From a Raspad / CS 1.6 tree that already has `vellum/metahook`:

```bat
build.bat
```

Or:

```bat
cmake -S . -B build/x86/Release -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_DIR=../metahook
cmake --build build/x86/Release --config Release --target MetaVoice
```

Copy `MetaVoice.dll` to `cstrike/metahook/plugins/` and add a line to `plugins.lst`:

```
MetaVoice.dll
```

## Roadmap

1. Apply `mv_gain` / `mv_boost` in software on 16-bit PCM **before** Speex (no Windows mixer).
2. Noise gate (`mv_gate`) so keyboard/breath is not sent.
3. Capture device pick if more than one mic is present.

Per-player **incoming** voice volume (Next Client style) is a playback-side feature, not this plugin.

## License

MIT
