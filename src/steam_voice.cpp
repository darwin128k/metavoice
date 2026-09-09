#include "steam_voice.h"

#include <metahook.h>
#include <opus.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define GS_RATE 24000
#define GS_FRAME 480
#define OPUS_BITRATE 32000
#define VPC_SETSAMPLERATE 11
#define VPC_OPUS_PLC 6
#define STEAMID64_INDIVIDUAL 0x0110000100000000ULL
#define MIN_OPUS_SPEECH_BYTES 8
#define MAX_FRAMES_PER_PKT 6
#define DAT_VOICE_STEAM_FLAG_RVA 0x0015E180u

enum {
    kVoiceOk = 0,
    kVoiceNotInitialized = 1,
    kVoiceDataCorrupted = 5
};

extern void MetaVoice_Log(const char *msg);
extern BYTE *MetaVoice_EngineBase(void);
extern int MetaVoice_PullCapturePcm(short *buf, int maxSamples);
extern unsigned MetaVoice_CaptureRate(void);
extern int MetaVoice_MicOpen(void);
extern float MetaVoice_CvarOr(const char *name, float fallback);

typedef void *(__cdecl *SteamUserFn)(void);
typedef unsigned long long (__cdecl *SteamGetIdCdeclFn)(void *self);

class SteamUserVoiceHook
{
public:
    int DecompressVoice(const void *comp, unsigned int compBytes, void *dst, unsigned int dstBytes,
                        unsigned int *written, unsigned int wantRate);
};

#define MAX_RX_SLOTS 8

typedef struct RxSlot_s {
    unsigned long long sid;
    OpusDecoder *dec;
    unsigned short seq;
    DWORD lastUsed;
} RxSlot;

static OpusEncoder *g_enc = NULL;
static unsigned short g_seq = 0;
static unsigned long long g_sid = STEAMID64_INDIVIDUAL | 1u;
static int g_recording = 0;
static int g_inGame = 0;
static int g_tweakMode = 0;
static int g_decompHooked = 0;
static RxSlot g_rx[MAX_RX_SLOTS];

static short g_in[8192];
static int g_inN = 0;
static double g_frac = 0.0;
static short g_pcm24[8192];
static int g_pcm24n = 0;
static unsigned char g_pkt[2048];
static unsigned int g_pktLen = 0;

static unsigned int Crc32(const unsigned char *data, unsigned int len)
{
    unsigned int crc = 0xFFFFFFFFu;
    unsigned int i, b, j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            b = crc & 1u;
            crc >>= 1;
            if (b) {
                crc ^= 0xEDB88320u;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void *SteamUserPtr(void)
{
    static SteamUserFn fn = NULL;
    static int tried = 0;
    HMODULE h;

    if (!tried) {
        tried = 1;
        h = GetModuleHandleA("steam_api.dll");
        if (h != NULL) {
            fn = (SteamUserFn)GetProcAddress(h, "SteamUser");
        }
    }
    if (fn == NULL) {
        return NULL;
    }
    return fn();
}

static unsigned long long ReadSteamId(void)
{
    HMODULE h;
    SteamGetIdCdeclFn getId;
    void *user = SteamUserPtr();

    h = GetModuleHandleA("steam_api.dll");
    if (h != NULL && user != NULL) {
        getId = (SteamGetIdCdeclFn)GetProcAddress(h, "SteamAPI_ISteamUser_GetSteamID");
        if (getId != NULL) {
            unsigned long long id = getId(user);
            if (id != 0) {
                return id;
            }
        }
    }
    if (user != NULL) {
        return STEAMID64_INDIVIDUAL | (((unsigned)(uintptr_t)user >> 4) & 0xFFFFFFFFu);
    }
    return STEAMID64_INDIVIDUAL | 1u;
}

static void ResetResample(void)
{
    g_inN = 0;
    g_frac = 0.0;
    g_pcm24n = 0;
    g_pktLen = 0;
    g_seq = 0;
}

static int EnsureEncoder(void)
{
    int err = 0;

    if (g_enc != NULL) {
        return 1;
    }
    g_enc = opus_encoder_create(GS_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (g_enc == NULL || err != OPUS_OK) {
        MetaVoice_Log("MetaVoice: opus_encoder_create failed");
        g_enc = NULL;
        return 0;
    }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
    MetaVoice_Log("MetaVoice: Opus VOIP encoder 24 kHz / 32 kbps");
    return 1;
}

static RxSlot *GetRxSlot(unsigned long long sid)
{
    int i;
    int freeIdx = -1;
    int lruIdx = 0;
    DWORD now = GetTickCount();
    int err = 0;

    for (i = 0; i < MAX_RX_SLOTS; i++) {
        if (g_rx[i].dec != NULL && g_rx[i].sid == sid) {
            g_rx[i].lastUsed = now;
            return &g_rx[i];
        }
        if (g_rx[i].dec == NULL && freeIdx < 0) {
            freeIdx = i;
        }
        if (g_rx[i].lastUsed < g_rx[lruIdx].lastUsed) {
            lruIdx = i;
        }
    }

    i = (freeIdx >= 0) ? freeIdx : lruIdx;
    if (g_rx[i].dec != NULL) {
        opus_decoder_destroy(g_rx[i].dec);
        g_rx[i].dec = NULL;
    }
    g_rx[i].dec = opus_decoder_create(GS_RATE, 1, &err);
    if (g_rx[i].dec == NULL || err != OPUS_OK) {
        MetaVoice_Log("MetaVoice: opus_decoder_create failed");
        g_rx[i].dec = NULL;
        return NULL;
    }
    g_rx[i].sid = sid;
    g_rx[i].seq = 0;
    g_rx[i].lastUsed = now;
    return &g_rx[i];
}

static float FrameRms(const short *s, int n)
{
    double acc = 0.0;
    int i;

    if (n <= 0) {
        return 0.0f;
    }
    for (i = 0; i < n; i++) {
        double v = (double)s[i];
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n) / 32768.0f;
}

static void AppendCapture(const short *src, int n, unsigned rate)
{
    int idx;
    double f;
    int a, b;
    int used;

    if (src == NULL || n <= 0 || rate == 0) {
        return;
    }
    if (g_inN + n > (int)(sizeof(g_in) / sizeof(g_in[0]))) {
        g_inN = 0;
        g_frac = 0.0;
    }
    memcpy(g_in + g_inN, src, (size_t)n * sizeof(short));
    g_inN += n;

    while (g_frac + 1.0 < (double)g_inN && g_pcm24n < (int)(sizeof(g_pcm24) / sizeof(g_pcm24[0]))) {
        idx = (int)g_frac;
        f = g_frac - (double)idx;
        a = g_in[idx];
        b = g_in[idx + 1];
        g_pcm24[g_pcm24n++] = (short)(a + (int)((b - a) * f));
        g_frac += (double)rate / (double)GS_RATE;
    }

    used = (int)g_frac;
    if (used > 0 && used < g_inN) {
        memmove(g_in, g_in + used, (size_t)(g_inN - used) * sizeof(short));
        g_inN -= used;
        g_frac -= (double)used;
    } else if (used >= g_inN) {
        g_inN = 0;
        g_frac = 0.0;
    }
}

static unsigned int BuildPacket(const unsigned char *payload, unsigned int payloadLen)
{
    unsigned int off = 0;
    unsigned int crc;

    if (payloadLen + 18u > sizeof(g_pkt)) {
        return 0;
    }
    memcpy(g_pkt + off, &g_sid, 8);
    off += 8;
    g_pkt[off++] = VPC_SETSAMPLERATE;
    g_pkt[off++] = (unsigned char)(GS_RATE & 0xFF);
    g_pkt[off++] = (unsigned char)((GS_RATE >> 8) & 0xFF);
    g_pkt[off++] = VPC_OPUS_PLC;
    g_pkt[off++] = (unsigned char)(payloadLen & 0xFF);
    g_pkt[off++] = (unsigned char)((payloadLen >> 8) & 0xFF);
    memcpy(g_pkt + off, payload, payloadLen);
    off += payloadLen;
    crc = Crc32(g_pkt, off);
    memcpy(g_pkt + off, &crc, 4);
    off += 4;
    return off;
}

static void EncodePending(void)
{
    unsigned char payload[1024];
    unsigned int pay = 0;
    unsigned char opusOut[400];
    int nEnc;
    int frames = 0;
    int speechFrames = 0;
    static DWORD lastLog = 0;

    g_pktLen = 0;
    if (!g_recording || !g_inGame) {
        return;
    }
    if (!EnsureEncoder()) {
        return;
    }

    /* Steam send path never calls VoiceRecord::GetMoreData — pull mic here. */
    if (MetaVoice_MicOpen()) {
        short tmp[2048];
        int n = MetaVoice_PullCapturePcm(tmp, 2048);
        unsigned rate = MetaVoice_CaptureRate();
        if (rate == 0) {
            rate = 11025;
        }
        if (n > 0) {
            AppendCapture(tmp, n, rate);
        }
    }

    while (g_pcm24n >= GS_FRAME && frames < MAX_FRAMES_PER_PKT && pay + 16 < sizeof(payload)) {
        if (FrameRms(g_pcm24, GS_FRAME) < 0.012f) {
            memmove(g_pcm24, g_pcm24 + GS_FRAME, (size_t)(g_pcm24n - GS_FRAME) * sizeof(short));
            g_pcm24n -= GS_FRAME;
            continue;
        }

        nEnc = opus_encode(g_enc, g_pcm24, GS_FRAME, opusOut, (int)sizeof(opusOut));
        memmove(g_pcm24, g_pcm24 + GS_FRAME, (size_t)(g_pcm24n - GS_FRAME) * sizeof(short));
        g_pcm24n -= GS_FRAME;
        if (nEnc < MIN_OPUS_SPEECH_BYTES) {
            continue;
        }
        if (pay + 4 + (unsigned)nEnc > sizeof(payload)) {
            break;
        }
        payload[pay] = (unsigned char)(nEnc & 0xFF);
        payload[pay + 1] = (unsigned char)((nEnc >> 8) & 0xFF);
        payload[pay + 2] = (unsigned char)(g_seq & 0xFF);
        payload[pay + 3] = (unsigned char)((g_seq >> 8) & 0xFF);
        g_seq++;
        memcpy(payload + pay + 4, opusOut, (size_t)nEnc);
        pay += 4 + (unsigned)nEnc;
        frames++;
        speechFrames++;
    }

    if (speechFrames == 0 || pay == 0) {
        return;
    }
    g_pktLen = BuildPacket(payload, pay);
    if (g_pktLen > 0) {
        DWORD now = GetTickCount();
        if (now - lastLog >= 1000u) {
            char dbg[192];
            lastLog = now;
            _snprintf(dbg, sizeof(dbg),
                      "MetaVoice: Steam Opus TX %u bytes frames=%d seq=%u",
                      g_pktLen, speechFrames, (unsigned)g_seq);
            dbg[sizeof(dbg) - 1] = '\0';
            MetaVoice_Log(dbg);
        }
    }
}

static int LooksLikeSteamVoice(const unsigned char *p, unsigned int n)
{
    unsigned int crc;
    unsigned int got;
    unsigned int pay;

    if (p == NULL || n < 18) {
        return 0;
    }
    if (p[8] != VPC_SETSAMPLERATE) {
        return 0;
    }
    if (p[11] != VPC_OPUS_PLC && p[11] != 4) {
        return 0;
    }
    pay = (unsigned)p[12] | ((unsigned)p[13] << 8);
    if (14u + pay + 4u != n) {
        return 0;
    }
    crc = Crc32(p, n - 4);
    memcpy(&got, p + n - 4, 4);
    return crc == got;
}

static int Resample24ToWanted(const short *src, int nSrc, short *dst, int maxDst, unsigned wantRate)
{
    double pos = 0.0;
    double step;
    int out = 0;

    if (wantRate == 0 || wantRate == GS_RATE) {
        if (nSrc > maxDst) {
            nSrc = maxDst;
        }
        memcpy(dst, src, (size_t)nSrc * sizeof(short));
        return nSrc;
    }
    step = (double)GS_RATE / (double)wantRate;
    while (pos + 1.0 < (double)nSrc && out < maxDst) {
        int i = (int)pos;
        double f = pos - (double)i;
        int a = src[i];
        int b = src[i + 1];
        dst[out++] = (short)(a + (int)((b - a) * f));
        pos += step;
    }
    return out;
}

static int DecodeSteamPayload(const unsigned char *comp, unsigned int compBytes, short *pcm24, int maxSamples)
{
    unsigned int off;
    unsigned int payLen;
    unsigned int end;
    int samples = 0;
    unsigned long long sid;
    RxSlot *slot;

    if (!LooksLikeSteamVoice(comp, compBytes)) {
        return -1;
    }
    memcpy(&sid, comp, 8);
    slot = GetRxSlot(sid);
    if (slot == NULL || slot->dec == NULL) {
        return -1;
    }

    payLen = (unsigned)comp[12] | ((unsigned)comp[13] << 8);
    off = 14;
    end = 14 + payLen;

    while (off + 4 <= end && samples + GS_FRAME <= maxSamples) {
        unsigned int frameBytes = (unsigned)comp[off] | ((unsigned)comp[off + 1] << 8);
        unsigned int seq = (unsigned)comp[off + 2] | ((unsigned)comp[off + 3] << 8);
        int got;

        off += 4;
        if (frameBytes == 0xFFFFu) {
            opus_decoder_ctl(slot->dec, OPUS_RESET_STATE);
            slot->seq = 0;
            break;
        }
        if (frameBytes == 0 || off + frameBytes > end) {
            break;
        }
        if (seq != slot->seq && slot->seq != 0) {
            /* PLC for gaps, capped. */
            int loss = (int)(seq - slot->seq);
            if (loss > 0 && loss < 10) {
                int i;
                for (i = 0; i < loss && samples + GS_FRAME <= maxSamples; i++) {
                    got = opus_decode(slot->dec, NULL, 0, pcm24 + samples, GS_FRAME, 0);
                    if (got > 0) {
                        samples += got;
                    }
                }
            }
        }
        slot->seq = (unsigned short)(seq + 1);
        got = opus_decode(slot->dec, comp + off, (int)frameBytes, pcm24 + samples, GS_FRAME, 0);
        off += frameBytes;
        if (got > 0) {
            samples += got;
        }
    }

    return samples;
}

int SteamUserVoiceHook::DecompressVoice(const void *comp, unsigned int compBytes, void *dst, unsigned int dstBytes,
                                        unsigned int *written, unsigned int wantRate)
{
    short pcm24[8192];
    short *out = (short *)dst;
    int maxOut = (int)(dstBytes / sizeof(short));
    int n24;
    int nOut;
    int i;
    int peak = 0;
    static DWORD lastLog = 0;

    (void)this;
    if (written != NULL) {
        *written = 0;
    }
    if (comp == NULL || dst == NULL || maxOut <= 0) {
        return kVoiceDataCorrupted;
    }

    n24 = DecodeSteamPayload((const unsigned char *)comp, compBytes, pcm24, 8192);
    if (n24 < 0) {
        return kVoiceDataCorrupted;
    }
    if (n24 == 0) {
        return kVoiceOk;
    }
    if (wantRate == 0) {
        wantRate = 11025;
    }
    nOut = Resample24ToWanted(pcm24, n24, out, maxOut, wantRate);

    {
        unsigned long long pktSid = 0;
        float gain = 1.0f;

        memcpy(&pktSid, comp, 8);
        /* Remote loudness = Options "Voice receive". Only own echo uses Voice monitor. */
        if (pktSid == g_sid) {
            gain = MetaVoice_CvarOr("mv_monitor", 1.0f);
            if (gain < 0.0f) {
                gain = 0.0f;
            }
            if (gain > 1.0f) {
                gain = 1.0f;
            }
        }

        for (i = 0; i < nOut; i++) {
            int v = (int)((float)out[i] * gain);
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            out[i] = (short)v;
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
    }

    if (written != NULL) {
        *written = (unsigned int)nOut * sizeof(short);
    }

    {
        DWORD now = GetTickCount();
        if (now - lastLog >= 1000u) {
            char dbg[160];
            lastLog = now;
            _snprintf(dbg, sizeof(dbg),
                      "MetaVoice: Steam Opus RX out=%d rate=%u peak=%d",
                      nOut, wantRate, peak);
            dbg[sizeof(dbg) - 1] = '\0';
            MetaVoice_Log(dbg);
        }
    }
    return kVoiceOk;
}

static void InstallDecompressHook(void)
{
    void *user;
    void **vt;
    DWORD oldProtect;
    char dbg[160];

    if (g_decompHooked) {
        return;
    }
    user = SteamUserPtr();
    if (user == NULL) {
        MetaVoice_Log("MetaVoice: DecompressVoice hook skipped (SteamUser NULL)");
        return;
    }
    vt = *(void ***)user;
    /* Slot 11 = DecompressVoice (vtable+0x2C). Do not touch GetVoice slots. */
    if (!VirtualProtect(&vt[11], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        MetaVoice_Log("MetaVoice: DecompressVoice protect failed");
        return;
    }
    {
        union {
            int (SteamUserVoiceHook::*mf)(const void *, unsigned int, void *, unsigned int, unsigned int *, unsigned int);
            void *p;
        } u;
        u.mf = &SteamUserVoiceHook::DecompressVoice;
        vt[11] = u.p;
    }
    VirtualProtect(&vt[11], sizeof(void *), oldProtect, &oldProtect);
    g_decompHooked = 1;
    _snprintf(dbg, sizeof(dbg), "MetaVoice: DecompressVoice hooked user=%p", user);
    dbg[sizeof(dbg) - 1] = '\0';
    MetaVoice_Log(dbg);
}

void SteamVoice_SetSteamFlag(int on)
{
    BYTE *base = MetaVoice_EngineBase();
    if (base != NULL) {
        *(BYTE *)(base + DAT_VOICE_STEAM_FLAG_RVA) = on ? 1 : 0;
    }
}

void SteamVoice_EnableSteamReceive(void)
{
    InstallDecompressHook();
}

void SteamVoice_PushPcm(const short *pcm, int nSamples, unsigned rate)
{
    if (!g_recording || !g_inGame || pcm == NULL || nSamples <= 0) {
        return;
    }
    if (rate == 0) {
        rate = 11025;
    }
    AppendCapture(pcm, nSamples, rate);
}

int SteamVoice_WantSteamSend(void)
{
    /* Options "Test Microphone" must stay on Speex loopback — Opus+speakers howls. */
    return (g_recording && g_inGame && !g_tweakMode) ? 1 : 0;
}

void SteamVoice_SetInGame(int inGame)
{
    g_inGame = inGame ? 1 : 0;
}

void SteamVoice_SetTweakMode(int on)
{
    g_tweakMode = on ? 1 : 0;
}

int SteamVoice_IsTweakMode(void)
{
    return g_tweakMode;
}

int SteamVoice_WritePacket(void *dst, unsigned int dstSz)
{
    if (!SteamVoice_WantSteamSend()) {
        return -1;
    }
    EncodePending();
    if (g_pktLen == 0) {
        return 0;
    }
    if (dst == NULL || dstSz < g_pktLen) {
        return 0;
    }
    memcpy(dst, g_pkt, g_pktLen);
    {
        unsigned int n = g_pktLen;
        g_pktLen = 0;
        return (int)n;
    }
}

void SteamVoice_OnEngage(void)
{
    char dbg[160];

    g_sid = ReadSteamId();
    EnsureEncoder();
    InstallDecompressHook();
    if (g_enc != NULL) {
        opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
    }
    ResetResample();
    g_recording = 1;
    _snprintf(dbg, sizeof(dbg), "MetaVoice: Steam Opus armed sid=%llu inGame=%d", g_sid, g_inGame);
    dbg[sizeof(dbg) - 1] = '\0';
    MetaVoice_Log(dbg);
}

void SteamVoice_OnRelease(void)
{
    g_recording = 0;
    g_pktLen = 0;
    ResetResample();
    if (g_enc != NULL) {
        opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
    }
}

void SteamVoice_Shutdown(void)
{
    int i;

    g_recording = 0;
    g_inGame = 0;
    g_tweakMode = 0;
    for (i = 0; i < MAX_RX_SLOTS; i++) {
        if (g_rx[i].dec != NULL) {
            opus_decoder_destroy(g_rx[i].dec);
            g_rx[i].dec = NULL;
        }
        g_rx[i].sid = 0;
        g_rx[i].seq = 0;
        g_rx[i].lastUsed = 0;
    }
    if (g_enc != NULL) {
        opus_encoder_destroy(g_enc);
        g_enc = NULL;
    }
    ResetResample();
}
