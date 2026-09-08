# MetaVoice

GoldSrc **voice capture** plugin for [MetaHookSv](https://github.com/hzqst/MetaHookSv).

[MetaAudio](https://github.com/darwin128k/MetaAudio) replaces how the game **plays** world sound (OpenAL).  
**MetaVoice** is the other half: how the game **records** your microphone and what you can change before Speex goes to the server.

It does **not** go through the Windows recording mixer for the settings we own. Capture stays on the engine WaveIn path (Bluetooth-safe: the mic opens only on `+voicerecord` / Test Microphone, then closes).

## What it does today

- Forces the native Speex + WaveIn path (RevEmu/Steam voice stub is skipped).
- Opens capture only when you talk or test the mic; closes it after, so Bluetooth can return to A2DP.
- Rebuilds codec + recorder after a level change (stock GoldSrc never does).
- Applies `mv_gain` / `mv_boost` in software on 16-bit PCM **before** Speex (Windows recording mixer is pinned to unity so it is not a second fader).
- Noise gate (`mv_gate`) zeros packets below the threshold so keyboard/breath is not sent.
- Registers those cvars for the Options UI (`FCVAR_ARCHIVE` → `config.cfg`).

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `mv_gain` | `1.0` | Transmit level, 0–1. Replaces Windows “Voice transmit volume”. |
| `mv_boost` | `0` | Extra gain on/off. Replaces Windows “Boost microphone gain”. |
| `mv_gate` | `0` | Noise gate threshold (0 = off, ~0.15 = tight). Applied on raw PCM. |

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

1. Capture device pick if a machine has more than one useful WaveIn input.

Per-player **incoming** voice volume (Next Client style) is a playback-side feature, not this plugin.

## License

MIT
