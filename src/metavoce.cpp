#include <metahook.h>
#include <cvardef.h>
#include <ivoicetweak.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "steam_voice.h"

static void Log(const char *msg)
{
    OutputDebugStringA(msg);
    FILE *f = fopen("metavoce.log", "a");
    if (f != NULL) {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        fprintf(f, "[%02d:%02d:%02d] %s\n", lt->tm_hour, lt->tm_min, lt->tm_sec, msg);
        fclose(f);
    }
}

void MetaVoice_Log(const char *msg)
{
    Log(msg);
}

/* hw.dll's Host_Init calls a codec-init helper as
 * FUN_01dc2c00("voice_speex", 1). That helper does:
 *
 *     iVar3 = SteamUser();
 *     DAT_01e5e180 = iVar3 != 0;
 *     if (DAT_01e5e180 || *param_1 == '\0') goto skip_native_init;
 *     ... opens the mic via VoiceRecord_DSound/VoiceRecord_WaveIn and
 *     LoadLibrary("voice_speex.dll") ...
 *
 * RevEmu's steamclient.dll answers SteamUser() with a non-null (but
 * voice-dead) interface, so the engine always believes Steam voice is
 * available and never runs its own, fully working DirectSound/WaveIn +
 * voice_speex.dll capture path. Patching TEST EAX,EAX (85 C0) into
 * XOR EAX,EAX (33 C0) right after that one CALL SteamUser forces the
 * "is Steam voice available" flag to always read false at this single
 * call site, without touching SteamUser() itself or any other call to
 * it elsewhere in the engine. Found via Ghidra on this exact hw.dll
 * build (Nov 2020); RVA is relative to GetEngineBase(), so relocation
 * doesn't matter. */

#define VOICE_PATCH_RVA 0x000C2C4Cu
static const unsigned char kExpected[2] = { 0x85, 0xC0 }; /* TEST EAX,EAX */
static const unsigned char kReplacement[2] = { 0x33, 0xC0 }; /* XOR EAX,EAX */

/* voice_dsound cvar_t.value (struct base at 0x01e5dfd4: name@0, string@4,
 * flags@8, value@0xC). Registered defaulting to "0", but something sets
 * it to 1 between LoadEngine and LoadClient on this box, which routes
 * voice through VoiceRecord_DSound. Its GetMoreData (vtable+0x14) bails
 * out to 0 bytes whenever the capture buffer's GetStatus() doesn't report
 * DSCBSTATUS_CAPTURING, which is a known weak spot for DirectSound
 * capture on Bluetooth headsets (WaveIn/MME is the far more commonly
 * supported legacy capture path on those drivers). Forcing this to 0.0
 * before Host_Init runs makes the engine take the VoiceRecord_WaveIn
 * path instead, regardless of whatever sets the cvar afterward -- the
 * recorder is only constructed once (or, with our reinit hook, each time
 * FUN_01dc2c00 reruns), always reading this value at that moment. */
#define DAT_VOICE_DSOUND_VALUE_RVA 0x0015DFE0u
/* BYTE DAT_01e5e180: "Steam voice available". FUN_01dc2c00 writes it
 * from SteamUser()!=0 (we XOR that one TEST so native Speex+WaveIn
 * still constructs). The per-frame send pump FUN_01dc3500 reads the
 * same byte first: if it is 1, it calls ISteamUser::GetVoice and never
 * VoiceRecord::GetMoreData / Speex. RevEmu leaves SteamUser non-null
 * and GetVoice empty, so the other player sees the talking icon and
 * hears silence — while Options VU still moves, because IVoiceTweak
 * reads WaveIn directly. Force the flag off wherever we already force
 * WaveIn. */
#define DAT_VOICE_STEAM_FLAG_RVA 0x0015E180u

/* DAT_01e5dfb4: a plain DWORD (not a cvar_t.value float -- it sits
 * before voice_dsound's own cvar_t struct and is read and passed
 * straight through as an integer sample rate) that Host_Init passes as
 * the requested nSamplesPerSec to both VoiceRecord_WaveIn/DSound::Init
 * and to the WAV-header writer for voice_recordtofile. Observed as
 * 11025 on this build. Bluetooth Hands-Free (HFP/HSP) links are natively
 * either 8000 Hz (CVSD) or 16000 Hz (mSBC); asking WaveIn to capture at
 * a rate the SCO link doesn't natively run at forces the driver to
 * resample on the fly, which is a plausible source of the short
 * (1-3 sample), thousands-of-times-per-recording gaps seen in
 * voice_micdata.wav. Forcing this to 8000 before every codec/recorder
 * (re)construction keeps the request consistent with what the link
 * actually natively delivers. */
#define DAT_VOICE_SAMPLERATE_RVA 0x0015DFB4u

/* Both VoiceRecord_WaveIn::Init(this, sampleRate) and
 * VoiceRecord_DSound::Init(this, sampleRate) open and immediately start
 * continuous capture (waveInOpen+waveInStart, or DirectSoundCaptureCreate
 * + IDirectSoundCaptureBuffer::Start) the moment Host_Init constructs
 * whichever class voice_dsound selects, and never stop until the process
 * exits. On a Bluetooth headset this permanently forces the Windows A2DP
 * (stereo music) profile down to HFP (mono headset) from the instant the
 * game starts, killing all game audio for the whole session -- not just
 * while talking.
 *
 * Neither class's own RecordStart/RecordStop (their vtable+0x8/+0xC) is a
 * safe place to defer this: WaveIn's just flips a software flag, but
 * DSound's are both empty no-ops (its capture buffer free-runs once
 * started). The one place both +voicerecord and the Options "Test
 * Microphone" button actually funnel through is FUN_01dc33a0 -- a plain
 * function, not a vtable slot, that calls vtable+0x8 on whichever
 * recorder object Host_Init created. Hooking that instead of the per-class
 * methods covers both classes with one hook and fires exactly when the
 * mic is actually wanted for the first time. */
#define VOICE_ENGAGE_RVA 0x000C33A0u

/* VoiceRecord_WaveIn::Init -- reached via a direct CALL, not virtual. */
#define VOICE_WAVEIN_INIT_RVA 0x000C4C50u

/* VoiceRecord_WaveIn vtable (RTTI ".?AVVoiceRecord_WaveIn@@"). GetMoreData
 * is vtable+0x14: thiscall (short *buf, int nSamples), ret 8, returns the
 * number of 16-bit samples written. Same slot on DSound. */
#define VOICE_WAVEIN_VTABLE_RVA 0x0011F138u
#define VOICE_GETMOREDATA_SLOT 0x14u

/* VoiceRecord_DSound::Init -- vtable+0x18, vtable resolved via its RTTI
 * Complete Object Locator (".?AVVoiceRecord_DSound@@") with Ghidra. */
#define VOICE_DSOUND_VTABLE_RVA 0x0011F0FCu
#define VOICE_DSOUND_VTABLE_INIT_SLOT 0x18u

/* Windows "Mic Boost" is +20 dB. Keep the same extra when mv_boost is on. */
#define MV_BOOST_LINEAR 10.0f
/* Slider 0–100 maps onto this peak-ish RMS band. 0 disables the gate. */
#define MV_GATE_MAX 0.15f
#define MV_GATE_HOLD 8

/* FUN_01dc33e0 -- the release-side counterpart called from -voicerecord
 * and from the Options "Stop Microphone Test" button: flushes the
 * codec's pending buffers but never touches the capture device itself.
 * Right place to bolt our own hardware close onto, since it's the one
 * spot both release paths reach. */
#define VOICE_RELEASE_RVA 0x000C33E0u

/* FUN_01dc2f00 -- full voice shutdown, guarded by DAT_024e4671 (set once
 * at the very first Host_Init and cleared again at the end of this same
 * function, so it's safe to call repeatedly). It destroys the codec
 * (DAT_024e46a8 plus a whole array of per-channel codec instances),
 * destroys and NULLs the recorder (DAT_024e46a4), and destroys the
 * IVoiceTweak wrapper -- called from Host_Shutdown on every level
 * change/disconnect, and from the per-frame voice tick if voice_enable
 * drops to 0. Since all of that is only ever constructed once, at the
 * process's first Host_Init, nothing recreates it afterward.
 * FUN_01dc3500 (the per-frame pump that actually pulls mic audio,
 * compresses it, and both sends it to the server and appends it to the
 * voice_recordtofile buffers) bails out immediately unless BOTH
 * DAT_024e46a4 and DAT_024e46a8 are non-NULL -- so once a single voice
 * shutdown has run, real transmission (not just the local open/close
 * mechanics) silently stops, without any error, for the rest of the
 * process. Rebuilding only the recorder (as an earlier version of this
 * fix did) isn't enough; the codec has to come back too.
 * Reconstructing the codec by hand means replicating the whole
 * LoadLibrary("voice_speex.dll")+CreateInterface+per-channel-array
 * dance FUN_01dc2c00 does, so instead we just call FUN_01dc2c00 itself
 * again -- the exact function Host_Init calls -- which rebuilds both
 * the codec and the recorder together, in the same order, and (since
 * our SteamGate patch is a permanent byte edit, not a one-shot flag)
 * takes the native path again every time. */
#define VOICE_SHUTDOWN_RVA 0x000C2F00u
#define VOICE_REINIT_RVA 0x000C2C00u
#define VOICE_SEND_PUMP_RVA 0x000C3500u
static const char kVoiceCodecName[] = "voice_speex";

typedef int (__fastcall *ClassInitFn)(void *pThis, void *edx, DWORD sampleRate);
typedef int (__fastcall *GetMoreDataFn)(void *pThis, void *edx, short *buf, int nSamples);
typedef int (__cdecl *EngageFn)(void);
typedef void (__cdecl *ShutdownFn)(void);
typedef int (__cdecl *ReinitVoiceFn)(const char *name, int flag);
typedef int (__cdecl *VoicePumpFn)(void *dst, unsigned int cb);

static ClassInitFn g_origWaveInInit = NULL;
static ClassInitFn g_origDSoundInit = NULL;
static GetMoreDataFn g_origWaveInGetMoreData = NULL;
static GetMoreDataFn g_origDSoundGetMoreData = NULL;
static EngageFn g_origEngage = NULL;
static EngageFn g_origRelease = NULL;
static ShutdownFn g_origShutdown = NULL;
static ReinitVoiceFn g_origReinitVoice = NULL;
static VoicePumpFn g_origVoicePump = NULL;

static void *g_pRecorder = NULL;
static DWORD g_sampleRate = 0;
static bool g_isDSound = false;
static bool g_hwOpened = false;
static bool g_recorderEverBuilt = false; /* true once we know how to rebuild (class + rate) */
static cl_enginefunc_t *g_eng = NULL;
static int g_gateOpen = 1;
static int g_gateHold = 0;
static float g_vuHoldL = 0.0f;
static float g_vuHoldR = 0.0f;

static void CloseWaveInHardware(void *pThis);
static void PinWindowsMixerUnity(void);
static void ForceWaveIn(void);
static int IsInGameMap(void);
int MetaVoice_PullCapturePcm(short *buf, int maxSamples);
unsigned MetaVoice_CaptureRate(void);
int MetaVoice_MicOpen(void);
BYTE *MetaVoice_EngineBase(void);

cl_exportfuncs_t gExportfuncs = { 0 };
mh_interface_t *g_pInterface = NULL;
metahook_api_t *g_pMetaHookAPI = NULL;
mh_enginesave_t *g_pMetaSave = NULL;

static float CvarValueOr(const char *name, float fallback)
{
    cvar_t *cv;

    if (g_eng == NULL || g_eng->pfnGetCvarPointer == NULL || name == NULL) {
        return fallback;
    }
    cv = g_eng->pfnGetCvarPointer(name);
    if (cv == NULL) {
        return fallback;
    }
    return cv->value;
}

float MetaVoice_CvarOr(const char *name, float fallback)
{
    return CvarValueOr(name, fallback);
}

static void ApplyNoiseGate(short *buf, int nSamples)
{
    float gate;
    float rms;
    double acc;
    int i;

    if (buf == NULL || nSamples <= 0) {
        return;
    }
    gate = CvarValueOr("mv_gate", 0.0f);
    if (gate < 0.0f) {
        gate = 0.0f;
    }
    if (gate > MV_GATE_MAX) {
        gate = MV_GATE_MAX;
    }
    if (gate < 0.0005f) {
        g_gateOpen = 1;
        g_gateHold = 0;
        return;
    }

    acc = 0.0;
    for (i = 0; i < nSamples; i++) {
        double s = (double)buf[i];
        acc += s * s;
    }
    rms = (float)sqrt(acc / (double)nSamples) / 32768.0f;

    if (rms >= gate) {
        g_gateOpen = 1;
        g_gateHold = MV_GATE_HOLD;
    } else if (g_gateHold > 0) {
        g_gateHold--;
    } else if (rms < gate * 0.65f) {
        g_gateOpen = 0;
    }

    if (!g_gateOpen) {
        memset(buf, 0, (size_t)nSamples * sizeof(short));
    }
}

static void PinWindowsMixerUnity(void)
{
    IVoiceTweak *tweak;

    if (g_eng == NULL) {
        return;
    }
    tweak = g_eng->pVoiceTweak;
    if (tweak == NULL || tweak->SetControlFloat == NULL) {
        return;
    }
    /* Capture stays at full hardware scale. Transmit/boost are applied
     * in ApplyCaptureGain on the PCM the engine already pulled. */
    tweak->SetControlFloat(MicrophoneVolume, 1.0f);
    tweak->SetControlFloat(MicBoost, 0.0f);
}

static void SetVuCvar(const char *name, float v)
{
    cvar_t *cv;

    if (g_eng == NULL || g_eng->pfnGetCvarPointer == NULL || name == NULL) {
        return;
    }
    if (v < 0.0f) {
        v = 0.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    cv = g_eng->pfnGetCvarPointer(name);
    if (cv != NULL) {
        cv->value = v;
    }
}

static void UpdateVuMeters(const short *buf, int nSamples)
{
    float peakL = 0.0f;
    float peakR = 0.0f;
    int i;

    if (buf == NULL || nSamples <= 0) {
        g_vuHoldL *= 0.86f;
        g_vuHoldR *= 0.86f;
        SetVuCvar("mv_vu_l", g_vuHoldL);
        SetVuCvar("mv_vu_r", g_vuHoldR);
        return;
    }
    if (nSamples >= 2) {
        for (i = 0; i + 1 < nSamples; i += 2) {
            float a = (float)buf[i];
            float b = (float)buf[i + 1];
            if (a < 0.0f) {
                a = -a;
            }
            if (b < 0.0f) {
                b = -b;
            }
            a /= 32768.0f;
            b /= 32768.0f;
            if (a > peakL) {
                peakL = a;
            }
            if (b > peakR) {
                peakR = b;
            }
        }
    } else {
        peakL = (float)buf[0];
        if (peakL < 0.0f) {
            peakL = -peakL;
        }
        peakL /= 32768.0f;
        peakR = peakL;
    }
    peakL *= 3.2f;
    peakR *= 3.2f;
    if (peakL > 1.0f) {
        peakL = 1.0f;
    }
    if (peakR > 1.0f) {
        peakR = 1.0f;
    }
    if (peakL > g_vuHoldL) {
        g_vuHoldL = peakL;
    } else {
        g_vuHoldL *= 0.86f;
    }
    if (peakR > g_vuHoldR) {
        g_vuHoldR = peakR;
    } else {
        g_vuHoldR *= 0.86f;
    }
    SetVuCvar("mv_vu_l", g_vuHoldL);
    SetVuCvar("mv_vu_r", g_vuHoldR);
}

static void ApplyCaptureGain(short *buf, int nSamples)
{
    float gain;
    float boost;
    float scale;
    int i;

    if (buf == NULL || nSamples <= 0) {
        UpdateVuMeters(NULL, 0);
        return;
    }

    ApplyNoiseGate(buf, nSamples);

    gain = CvarValueOr("mv_gain", 1.0f);
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    if (gain > 1.0f) {
        gain = 1.0f;
    }

    boost = 1.0f;
    if (CvarValueOr("mv_boost", 0.0f) > 0.5f) {
        boost = MV_BOOST_LINEAR;
    }

    scale = gain * boost;
    if (scale != 1.0f) {
        for (i = 0; i < nSamples; i++) {
            int v = (int)((float)buf[i] * scale);
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            buf[i] = (short)v;
        }
    }
    UpdateVuMeters(buf, nSamples);
}

int MetaVoice_PullCapturePcm(short *buf, int maxSamples)
{
    int n = 0;

    if (buf == NULL || maxSamples <= 0 || !g_hwOpened || g_pRecorder == NULL) {
        return 0;
    }
    if (g_isDSound && g_origDSoundGetMoreData != NULL) {
        n = g_origDSoundGetMoreData(g_pRecorder, NULL, buf, maxSamples);
    } else if (g_origWaveInGetMoreData != NULL) {
        n = g_origWaveInGetMoreData(g_pRecorder, NULL, buf, maxSamples);
    }
    ApplyCaptureGain(buf, n);
    return n;
}

unsigned MetaVoice_CaptureRate(void)
{
    return (unsigned)g_sampleRate;
}

int MetaVoice_MicOpen(void)
{
    return g_hwOpened ? 1 : 0;
}

BYTE *MetaVoice_EngineBase(void)
{
    if (g_pMetaHookAPI == NULL) {
        return NULL;
    }
    return (BYTE *)g_pMetaHookAPI->GetEngineBase();
}

static int __fastcall Hook_WaveInGetMoreData(void *pThis, void *edx, short *buf, int nSamples)
{
    int n;

    if (g_origWaveInGetMoreData == NULL) {
        return 0;
    }
    n = g_origWaveInGetMoreData(pThis, edx, buf, nSamples);
    ForceWaveIn();
    ApplyCaptureGain(buf, n);
    if (n > 0) {
        SteamVoice_PushPcm(buf, n, (unsigned)g_sampleRate);
    }
    return n;
}

static int __fastcall Hook_DSoundGetMoreData(void *pThis, void *edx, short *buf, int nSamples)
{
    int n;

    if (g_origDSoundGetMoreData == NULL) {
        return 0;
    }
    n = g_origDSoundGetMoreData(pThis, edx, buf, nSamples);
    ForceWaveIn();
    ApplyCaptureGain(buf, n);
    if (n > 0) {
        SteamVoice_PushPcm(buf, n, (unsigned)g_sampleRate);
    }
    return n;
}

static void ForceWaveIn(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    *(float *)(base + DAT_VOICE_DSOUND_VALUE_RVA) = 0.0f;
}

static void PatchVoiceSpeexGate(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    BYTE *target = base + VOICE_PATCH_RVA;
    DWORD oldProtect;

    if (memcmp(target, kExpected, sizeof(kExpected)) != 0) {
        Log("MetaVoice: Speex SteamUser patch skipped (hw.dll bytes mismatch)");
        return;
    }

    if (!VirtualProtect(target, sizeof(kReplacement), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    memcpy(target, kReplacement, sizeof(kReplacement));

    VirtualProtect(target, sizeof(kReplacement), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(kReplacement));
    Log("MetaVoice: Speex SteamUser patch applied");
}

static int __fastcall Hook_WaveInInit(void *pThis, void *edx, DWORD sampleRate)
{
    char dbg[128];
    (void)edx;
    g_pRecorder = pThis;
    g_sampleRate = sampleRate;
    g_isDSound = false;
    g_hwOpened = false;
    g_recorderEverBuilt = true;
    _snprintf(dbg, sizeof(dbg), "MetaVoice: deferred WaveIn Init this=%p rate=%lu", pThis, (unsigned long)sampleRate);
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);
    return 1;
}

static int __fastcall Hook_DSoundInit(void *pThis, void *edx, DWORD sampleRate)
{
    char dbg[128];
    (void)edx;
    g_pRecorder = pThis;
    g_sampleRate = sampleRate;
    g_isDSound = true;
    g_hwOpened = false;
    g_recorderEverBuilt = true;
    _snprintf(dbg, sizeof(dbg), "MetaVoice: deferred DSound Init this=%p rate=%lu", pThis, (unsigned long)sampleRate);
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);
    return 1;
}

/* Wraps FUN_01dc2c00 itself (rather than just poking the cvar once in
 * LoadEngine) so voice_dsound reads back 0.0 immediately before every
 * single invocation -- the natural one from Host_Init, and our own
 * explicit re-invocation from Hook_Engage below -- regardless of
 * whatever sets the cvar back to 1 in between. */
static int __cdecl Hook_ReinitVoice(const char *name, int flag)
{
    ForceWaveIn();
    return g_origReinitVoice(name, flag);
}

static int __cdecl Hook_Engage(void)
{
    char dbg[160];
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Engage recorder=%p isDSound=%d hwOpened=%d everBuilt=%d",
              g_pRecorder, (int)g_isDSound, (int)g_hwOpened, (int)g_recorderEverBuilt);
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);
    ForceWaveIn();

    if (g_pRecorder == NULL && g_recorderEverBuilt && g_origReinitVoice != NULL) {
        /* A voice shutdown (level change/disconnect) destroyed both the
         * codec and the recorder and nothing else recreates them. Safe
         * to redo the whole Host_Init voice setup here: we only reach
         * this because the player just pressed +voicerecord/Test
         * Microphone, so the game is definitely alive and running, not
         * exiting. Hook_WaveInInit/Hook_DSoundInit run synchronously
         * inside this call (Init is what they replace) and set
         * g_pRecorder/g_hwOpened for us as a side effect. Going through
         * Hook_ReinitVoice (not the raw trampoline) so voice_dsound gets
         * forced back to 0 here too. */
        int reinitOk = Hook_ReinitVoice(kVoiceCodecName, 1);
        _snprintf(dbg, sizeof(dbg), "MetaVoice: lazily reran voice codec+recorder setup, ok=%d recorder=%p",
                  reinitOk, g_pRecorder);
        dbg[sizeof(dbg) - 1] = '\0';
        Log(dbg);
    }

    if (!g_hwOpened && g_pRecorder != NULL) {
        if (g_isDSound && g_origDSoundInit != NULL) {
            Log("MetaVoice: opening DSound hardware now");
            g_origDSoundInit(g_pRecorder, NULL, g_sampleRate);
        } else if (!g_isDSound && g_origWaveInInit != NULL) {
            Log("MetaVoice: opening WaveIn hardware now");
            g_origWaveInInit(g_pRecorder, NULL, g_sampleRate);
        }
        g_hwOpened = true;
        PinWindowsMixerUnity();
    }

    /* Steam flag must be 0 for Engage: otherwise hw skips VoiceRecord::RecordStart
     * and WaveIn never arms — mic stays dead for K and Test Microphone. */
    SteamVoice_SetSteamFlag(0);
    SteamVoice_SetInGame(IsInGameMap());
    SteamVoice_OnEngage();
    int ret = g_origEngage() & 0xFF;
    if (IsInGameMap()) {
        SteamVoice_SetSteamFlag(1);
    }
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Engage orig returned %d inGame=%d", ret, IsInGameMap());
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);
    return ret;
}

/* Reverse of VoiceRecord_DSound::Init (FUN_01dc47e0), reconstructed from
 * its decompile: capture buffer pointer lives at this+0xC, the
 * IDirectSoundCapture8 pointer at this+8, the wrap-notify event handle at
 * this+0x18. Both are plain COM interfaces (stdcall vtables): Stop is
 * IDirectSoundCaptureBuffer8 vtable+0x28 (Start, called by Init, is
 * +0x24), Release is the standard COM vtable+8 on both interfaces. There
 * is no engine-provided counterpart for this -- RecordStart/RecordStop
 * (vtable+0x8/+0xC) are both no-ops for this class, so nothing ever shuts
 * the capture buffer off once Init starts it looping. */
typedef HRESULT(__stdcall *StdcallVoidFn)(void *pThis);

static void CloseDSoundHardware(void *pThis)
{
    BYTE *obj = (BYTE *)pThis;
    void *pBuffer = *(void **)(obj + 0xC);
    void *pCapture = *(void **)(obj + 8);
    HANDLE hEvent = *(HANDLE *)(obj + 0x18);

    if (pBuffer != NULL) {
        void **vtable = *(void ***)pBuffer;
        ((StdcallVoidFn)vtable[0x28 / sizeof(void *)])(pBuffer);
        ((StdcallVoidFn)vtable[8 / sizeof(void *)])(pBuffer);
        *(void **)(obj + 0xC) = NULL;
    }
    if (pCapture != NULL) {
        void **vtable = *(void ***)pCapture;
        ((StdcallVoidFn)vtable[8 / sizeof(void *)])(pCapture);
        *(void **)(obj + 8) = NULL;
    }
    if (hEvent != NULL) {
        CloseHandle(hEvent);
        *(HANDLE *)(obj + 0x18) = NULL;
    }
}

/* Reverse of VoiceRecord_WaveIn::Init (FUN_01dc4c50): the HWAVEIN handle
 * lives at this+0x10FC, and 15 WAVEHDR buffers start at this+4 (each
 * WAVEHDR is 0x20 bytes on this build, matching waveInPrepareHeader's
 * size argument). waveInReset flushes and returns all pending buffers;
 * each must be unprepared before waveInClose is legal. */
static void CloseWaveInHardware(void *pThis)
{
    BYTE *obj = (BYTE *)pThis;
    HWAVEIN hwi = *(HWAVEIN *)(obj + 0x10FC);
    int i;

    if (hwi == NULL) {
        return;
    }

    waveInReset(hwi);
    for (i = 0; i < 15; i++) {
        waveInUnprepareHeader(hwi, (LPWAVEHDR)(obj + 4 + i * 0x20), sizeof(WAVEHDR));
    }
    waveInClose(hwi);
    *(HWAVEIN *)(obj + 0x10FC) = NULL;
}

static int __cdecl Hook_Release(void)
{
    /* Same as Engage: native RecordStop only runs when Steam flag is off. */
    SteamVoice_SetSteamFlag(0);
    int ret = g_origRelease() & 0xFF;
    char dbg[128];
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Release recorder=%p isDSound=%d hwOpened=%d origReturned=%d",
              g_pRecorder, (int)g_isDSound, (int)g_hwOpened, ret);
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);

    if (g_hwOpened && g_pRecorder != NULL) {
        if (g_isDSound) {
            Log("MetaVoice: releasing DSound hardware");
            CloseDSoundHardware(g_pRecorder);
        } else {
            Log("MetaVoice: releasing WaveIn hardware");
            CloseWaveInHardware(g_pRecorder);
        }
        g_hwOpened = false;
    }
    SteamVoice_OnRelease();
    if (IsInGameMap()) {
        SteamVoice_SetSteamFlag(1);
    }
    return ret;
}

/* FUN_01dc2f00 also runs on the final process shutdown (quitting the
 * game entirely), not just on level change/disconnect. Rebuilding the
 * recorder from inside this hook -- as an earlier version of this fix
 * did -- meant allocating a fresh DirectSound/WaveIn object and reopening
 * hardware right as the process was tearing down, which could hang
 * cstrike.exe on exit instead of letting it close. So this hook only
 * closes our own hardware (if open) and mirrors the engine's own
 * teardown; Hook_Engage is responsible for lazily rebuilding the
 * recorder the next time it's actually needed, which only happens while
 * the game is demonstrably still running. */
static void __cdecl Hook_Shutdown(void)
{
    char dbg[128];
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Shutdown recorder=%p isDSound=%d hwOpened=%d",
              g_pRecorder, (int)g_isDSound, (int)g_hwOpened);
    dbg[sizeof(dbg) - 1] = '\0';
    Log(dbg);

    if (g_pRecorder != NULL && g_hwOpened) {
        if (g_isDSound) {
            CloseDSoundHardware(g_pRecorder);
        } else {
            CloseWaveInHardware(g_pRecorder);
        }
    }

    g_origShutdown();

    g_pRecorder = NULL;
    g_hwOpened = false;
    SteamVoice_Shutdown();
}

static int IsInGameMap(void)
{
    const char *lvl;

    if (g_eng == NULL || g_eng->pfnGetLevelName == NULL) {
        return 0;
    }
    lvl = g_eng->pfnGetLevelName();
    if (lvl == NULL || lvl[0] == '\0') {
        return 0;
    }
    /* Menu / not connected often reports ".bsp" empty stem or just extension. */
    if (lvl[0] == '.' || (lvl[0] == 'c' && lvl[1] == '\0')) {
        return 0;
    }
    return 1;
}

static void __cdecl Hook_HUD_Frame(double time)
{
    if (IsInGameMap() && !SteamVoice_IsTweakMode()) {
        SteamVoice_SetInGame(1);
        SteamVoice_EnableSteamReceive();
        /* Keep Steam receive path armed without requiring a prior +voicerecord. */
        SteamVoice_SetSteamFlag(1);
    }
    if (gExportfuncs.HUD_Frame != NULL) {
        gExportfuncs.HUD_Frame(time);
    }
}

static int __cdecl Hook_VoicePump(void *dst, unsigned int cb)
{
    int n;

    SteamVoice_SetInGame(IsInGameMap());
    n = SteamVoice_WritePacket(dst, cb);
    if (n > 0) {
        return n;
    }
    /* n == 0: armed but silence this tick — do not fall back to Speex
     * (would flash a second codec). n < 0: mic test / menu → Speex. */
    if (n == 0 && SteamVoice_WantSteamSend()) {
        return 0;
    }
    if (g_origVoicePump == NULL) {
        return 0;
    }
    return g_origVoicePump(dst, cb);
}

static void InstallDeferredMicHooks(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    void *engageAddr = base + VOICE_ENGAGE_RVA;
    void *releaseAddr = base + VOICE_RELEASE_RVA;
    void *waveInInitAddr = base + VOICE_WAVEIN_INIT_RVA;
    void *shutdownAddr = base + VOICE_SHUTDOWN_RVA;
    void *reinitAddr = base + VOICE_REINIT_RVA;
    void *pumpAddr = base + VOICE_SEND_PUMP_RVA;
    BYTE **dsoundVtableSlot = (BYTE **)(base + VOICE_DSOUND_VTABLE_RVA + VOICE_DSOUND_VTABLE_INIT_SLOT);
    BYTE **waveInGetMore = (BYTE **)(base + VOICE_WAVEIN_VTABLE_RVA + VOICE_GETMOREDATA_SLOT);
    BYTE **dsoundGetMore = (BYTE **)(base + VOICE_DSOUND_VTABLE_RVA + VOICE_GETMOREDATA_SLOT);
    DWORD oldProtect;

    g_pMetaHookAPI->InlineHook(reinitAddr, (void *)Hook_ReinitVoice, (void **)&g_origReinitVoice);
    g_pMetaHookAPI->InlineHook(pumpAddr, (void *)Hook_VoicePump, (void **)&g_origVoicePump);

    if (g_pMetaHookAPI->InlineHook(engageAddr, (void *)Hook_Engage, (void **)&g_origEngage) == NULL) {
        return;
    }

    g_pMetaHookAPI->InlineHook(releaseAddr, (void *)Hook_Release, (void **)&g_origRelease);
    g_pMetaHookAPI->InlineHook(waveInInitAddr, (void *)Hook_WaveInInit, (void **)&g_origWaveInInit);

    if (VirtualProtect(dsoundVtableSlot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_origDSoundInit = (ClassInitFn)*dsoundVtableSlot;
        *dsoundVtableSlot = (BYTE *)Hook_DSoundInit;
        VirtualProtect(dsoundVtableSlot, sizeof(void *), oldProtect, &oldProtect);
    }

    if (VirtualProtect(waveInGetMore, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_origWaveInGetMoreData = (GetMoreDataFn)*waveInGetMore;
        *waveInGetMore = (BYTE *)Hook_WaveInGetMoreData;
        VirtualProtect(waveInGetMore, sizeof(void *), oldProtect, &oldProtect);
    }

    if (VirtualProtect(dsoundGetMore, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_origDSoundGetMoreData = (GetMoreDataFn)*dsoundGetMore;
        *dsoundGetMore = (BYTE *)Hook_DSoundGetMoreData;
        VirtualProtect(dsoundGetMore, sizeof(void *), oldProtect, &oldProtect);
    }

    g_pMetaHookAPI->InlineHook(shutdownAddr, (void *)Hook_Shutdown, (void **)&g_origShutdown);
}

void IPluginsV4::Init(metahook_api_t *pAPI, mh_interface_t *pInterface, mh_enginesave_t *pSave)
{
    g_pInterface = pInterface;
    g_pMetaHookAPI = pAPI;
    g_pMetaSave = pSave;
}

void IPluginsV4::Shutdown(void)
{
}

static int (*g_origStartVoiceTweak)(void) = NULL;
static void (*g_origEndVoiceTweak)(void) = NULL;
static float g_savedOtherSpeaker = 1.0f;
static int g_haveSavedOtherSpeaker = 0;

static int Hook_StartVoiceTweak(void)
{
    IVoiceTweak *tweak;
    int ok;

    SteamVoice_SetTweakMode(1);
    /* Speex local monitor + Steam flag off avoids Opus feedback on laptop speakers. */
    SteamVoice_SetSteamFlag(0);

    /* Voice receive IS OtherSpeakerScale — never overwrite it with mv_monitor
     * (that made the Receive slider jump during Test Microphone). */
    tweak = (g_eng != NULL) ? g_eng->pVoiceTweak : NULL;
    g_haveSavedOtherSpeaker = 0;
    if (tweak != NULL && tweak->GetControlFloat != NULL) {
        g_savedOtherSpeaker = tweak->GetControlFloat(OtherSpeakerScale);
        g_haveSavedOtherSpeaker = 1;
    }

    ok = (g_origStartVoiceTweak != NULL) ? g_origStartVoiceTweak() : 0;

    if (g_haveSavedOtherSpeaker && tweak != NULL && tweak->SetControlFloat != NULL) {
        tweak->SetControlFloat(OtherSpeakerScale, g_savedOtherSpeaker);
    }

    Log("MetaVoice: VoiceTweak start");
    return ok;
}

static void Hook_EndVoiceTweak(void)
{
    IVoiceTweak *tweak;

    if (g_origEndVoiceTweak != NULL) {
        g_origEndVoiceTweak();
    }

    tweak = (g_eng != NULL) ? g_eng->pVoiceTweak : NULL;
    if (g_haveSavedOtherSpeaker && tweak != NULL && tweak->SetControlFloat != NULL) {
        tweak->SetControlFloat(OtherSpeakerScale, g_savedOtherSpeaker);
    }
    g_haveSavedOtherSpeaker = 0;

    SteamVoice_SetTweakMode(0);
    if (IsInGameMap()) {
        SteamVoice_SetSteamFlag(1);
    }
    Log("MetaVoice: VoiceTweak end");
}

static void HookVoiceTweakApi(void)
{
    IVoiceTweak *tweak;

    if (g_eng == NULL || g_eng->pVoiceTweak == NULL) {
        return;
    }
    tweak = g_eng->pVoiceTweak;
    if (tweak->StartVoiceTweakMode == Hook_StartVoiceTweak) {
        return;
    }
    g_origStartVoiceTweak = tweak->StartVoiceTweakMode;
    g_origEndVoiceTweak = tweak->EndVoiceTweakMode;
    tweak->StartVoiceTweakMode = Hook_StartVoiceTweak;
    tweak->EndVoiceTweakMode = Hook_EndVoiceTweak;
    Log("MetaVoice: IVoiceTweak hooked");
}

static void RegisterGainCvars(void)
{
    if (g_eng == NULL || g_eng->pfnRegisterVariable == NULL) {
        return;
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_gain") == NULL) {
        g_eng->pfnRegisterVariable("mv_gain", "1.0", FCVAR_ARCHIVE);
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_boost") == NULL) {
        g_eng->pfnRegisterVariable("mv_boost", "0", FCVAR_ARCHIVE);
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_gate") == NULL) {
        g_eng->pfnRegisterVariable("mv_gate", "0", FCVAR_ARCHIVE);
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_monitor") == NULL) {
        g_eng->pfnRegisterVariable("mv_monitor", "1.0", FCVAR_ARCHIVE);
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_vu_l") == NULL) {
        g_eng->pfnRegisterVariable("mv_vu_l", "0", 0);
    }
    if (g_eng->pfnGetCvarPointer == NULL || g_eng->pfnGetCvarPointer("mv_vu_r") == NULL) {
        g_eng->pfnRegisterVariable("mv_vu_r", "0", 0);
    }
}

void IPluginsV4::LoadEngine(cl_enginefunc_t *pEngineFuncs)
{
    g_eng = pEngineFuncs;
    PatchVoiceSpeexGate();
    InstallDeferredMicHooks();
    SteamVoice_EnableSteamReceive();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t *pExportFuncs)
{
    memcpy(&gExportfuncs, pExportFuncs, sizeof(gExportfuncs));
    pExportFuncs->HUD_Frame = Hook_HUD_Frame;
    RegisterGainCvars();
    HookVoiceTweakApi();
}

void IPluginsV4::ExitGame(int iResult)
{
    (void)iResult;
}

const char *IPluginsV4::GetVersion(void)
{
    return "0.4.9";
}

EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);
