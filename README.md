# MetaVoice

GoldSrc **voice** plugin for [MetaHookSv](https://github.com/hzqst/MetaHookSv).

[MetaAudio](https://github.com/darwin128k/MetaAudio) replaces how the game **plays** world sound (OpenAL Soft).  
**MetaVoice** is the other half: microphone capture, encoding, and Steam-compatible voice packets on clients where RevEmu’s `ISteamUser::GetVoice` is empty.

Levels and gates live in **Options → Voice** (leaflet). MetaVoice is the implementation behind those controls — it does not bake hidden boosts.

## Speex vs Opus

Speex is **not** removed.

| Situation | Codec |
|---|---|
| In-map talk (`+voicerecord`) | **Opus** Steam Voice packets (TX + RX via `DecompressVoice`) |
| Options → Test Microphone / menu | **Speex** local loopback (`voice_speex.dll`) |
| Engine codec object after level change | Still rebuilt as `voice_speex` (Host_Init path) |

Why Speex stays for the mic test: Opus + speakers next to a laptop mic howls hard. Speex loopback matches stock GoldSrc tweak mode; **Voice monitor** sets how loud that echo is.

In-game peer voice needs Opus so other Raspad / Steam-format clients can hear you. RevEmu never fills `GetVoice`, so MetaVoice encodes PCM itself and hooks receive decompress (per-SteamID Opus decoders).

## What it does

- Patches the one SteamUser check that would skip WaveIn + `voice_speex` construction under RevEmu.
- Defers WaveIn open until Engage (`+voicerecord` / Test Mic); closes on Release (Bluetooth can return to A2DP).
- Rebuilds codec + recorder after voice shutdown / level change (stock never recreates them).
- In-map: pulls capture PCM, encodes Opus Steam packets, keeps Steam receive path armed.
- Mic test: Speex path; does not send Opus to the network.
- Applies UI-owned capture gain / boost / gate on PCM before encode.
- Own-voice monitor uses `mv_monitor`; other players use stock **Voice receive**.

Do not load **VoiceFix.dll** and **MetaVoice.dll** together.

## Options → Voice (mixer)

| Control | Cvar | Role |
|---|---|---|
| Voice transmit volume | `mv_gain` | Mic send level (0–1) |
| Boost microphone gain | `mv_boost` | Extra capture gain on/off |
| Voice receive volume | engine | How loud **others** are |
| Noise gate | `mv_gate` | Drop quiet PCM (0 = off) |
| Voice monitor | `mv_monitor` | How loud **you** hear yourself (test / echo) |

VU meters read `mv_vu_l` / `mv_vu_r` (not archived).

Windows recording mixer is pinned to unity for the settings MetaVoice owns, so transmit is not double-faded.

## Build

Needs MetaHookSv headers (`../metahook` in a Raspad / CS tree):

```bat
build.bat
```

Or:

```bat
cmake -S . -B build/x86/Release -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_DIR=../metahook
cmake --build build/x86/Release --config Release --target MetaVoice
```

Install:

```
cstrike/metahook/plugins/MetaVoice.dll
```

and a `plugins.lst` line:

```
MetaVoice.dll
```

Pair with **MetaAudio** for playback. Leaflet owns the Voice tab layout (`Voice monitor` after Noise gate).

## Target

Tested on Steam GoldSrc **Aug 3 2020 / 8684** (Raspad / RevEmu). Engine RVAs are for that `hw.dll` build.

## License

MIT
