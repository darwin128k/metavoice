#include <metahook.h>
#include <cvardef.h>
#include <ivoicetweak.h>
#include <mmsystem.h>
#include <stdio.h>
#include <time.h>

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
static const char kVoiceCodecName[] = "voice_speex";

typedef int (__fastcall *ClassInitFn)(void *pThis, void *edx, DWORD sampleRate);
typedef int (__fastcall *GetMoreDataFn)(void *pThis, void *edx, short *buf, int nSamples);
typedef int (__cdecl *EngageFn)(void);
typedef void (__cdecl *ShutdownFn)(void);
typedef int (__cdecl *ReinitVoiceFn)(const char *name, int flag);

static ClassInitFn g_origWaveInInit = NULL;
static ClassInitFn g_origDSoundInit = NULL;
static GetMoreDataFn g_origWaveInGetMoreData = NULL;
static GetMoreDataFn g_origDSoundGetMoreData = NULL;
static EngageFn g_origEngage = NULL;
static EngageFn g_origRelease = NULL;
static ShutdownFn g_origShutdown = NULL;
static ReinitVoiceFn g_origReinitVoice = NULL;

static void *g_pRecorder = NULL;
static DWORD g_sampleRate = 0;
static bool g_isDSound = false;
static bool g_hwOpened = false;
static bool g_recorderEverBuilt = false; /* true once we know how to rebuild (class + rate) */
static cl_enginefunc_t *g_eng = NULL;

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

static void ApplyCaptureGain(short *buf, int nSamples)
{
    float gain;
    float boost;
    float scale;
    int i;

    if (buf == NULL || nSamples <= 0) {
        return;
    }

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
    if (scale == 1.0f) {
        return;
    }

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

static int __fastcall Hook_WaveInGetMoreData(void *pThis, void *edx, short *buf, int nSamples)
{
    int n;

    if (g_origWaveInGetMoreData == NULL) {
        return 0;
    }
    n = g_origWaveInGetMoreData(pThis, edx, buf, nSamples);
    ApplyCaptureGain(buf, n);
    return n;
}

static int __fastcall Hook_DSoundGetMoreData(void *pThis, void *edx, short *buf, int nSamples)
{
    int n;

    if (g_origDSoundGetMoreData == NULL) {
        return 0;
    }
    n = g_origDSoundGetMoreData(pThis, edx, buf, nSamples);
    ApplyCaptureGain(buf, n);
    return n;
}

static void ForceWaveIn(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    *(float *)(base + DAT_VOICE_DSOUND_VALUE_RVA) = 0.0f;
    /* Tried forcing DAT_VOICE_SAMPLERATE_RVA to 8000 (HFP's native CVSD
     * rate) on the theory that a mismatched WaveIn request rate was
     * behind the choppy capture -- it made things drastically worse
     * (max amplitude ~9, i.e. near silence, vs ~3700 at 11025), so this
     * device apparently does not do real 8kHz WaveIn capture at all.
     * Leaving the engine's own default (11025) alone. */
}

static void PatchVoiceSpeexGate(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    BYTE *target = base + VOICE_PATCH_RVA;
    DWORD oldProtect;

    if (memcmp(target, kExpected, sizeof(kExpected)) != 0) {
        return;
    }

    if (!VirtualProtect(target, sizeof(kReplacement), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    memcpy(target, kReplacement, sizeof(kReplacement));

    VirtualProtect(target, sizeof(kReplacement), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(kReplacement));
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
    int ret = g_origEngage();
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Engage orig returned %d", ret);
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
    int ret = g_origRelease();
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
}

static void InstallDeferredMicHooks(void)
{
    BYTE *base = (BYTE *)g_pMetaHookAPI->GetEngineBase();
    void *engageAddr = base + VOICE_ENGAGE_RVA;
    void *releaseAddr = base + VOICE_RELEASE_RVA;
    void *waveInInitAddr = base + VOICE_WAVEIN_INIT_RVA;
    void *shutdownAddr = base + VOICE_SHUTDOWN_RVA;
    void *reinitAddr = base + VOICE_REINIT_RVA;
    BYTE **dsoundVtableSlot = (BYTE **)(base + VOICE_DSOUND_VTABLE_RVA + VOICE_DSOUND_VTABLE_INIT_SLOT);
    BYTE **waveInGetMore = (BYTE **)(base + VOICE_WAVEIN_VTABLE_RVA + VOICE_GETMOREDATA_SLOT);
    BYTE **dsoundGetMore = (BYTE **)(base + VOICE_DSOUND_VTABLE_RVA + VOICE_GETMOREDATA_SLOT);
    DWORD oldProtect;

    g_pMetaHookAPI->InlineHook(reinitAddr, (void *)Hook_ReinitVoice, (void **)&g_origReinitVoice);

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
}

void IPluginsV4::LoadEngine(cl_enginefunc_t *pEngineFuncs)
{
    g_eng = pEngineFuncs;
    PatchVoiceSpeexGate();
    InstallDeferredMicHooks();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t *pExportFuncs)
{
    memcpy(&gExportfuncs, pExportFuncs, sizeof(gExportfuncs));
    RegisterGainCvars();
}

void IPluginsV4::ExitGame(int iResult)
{
    (void)iResult;
}

const char *IPluginsV4::GetVersion(void)
{
    return "0.2.0";
}

EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);
