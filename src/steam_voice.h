#pragma once

void SteamVoice_OnEngage(void);
void SteamVoice_OnRelease(void);
void SteamVoice_Shutdown(void);
void SteamVoice_PushPcm(const short *pcm, int nSamples, unsigned rate);
/* Packet size; 0 = silence; -1 = Speex path (menu / mic test). */
int SteamVoice_WritePacket(void *dst, unsigned int dstSz);
int SteamVoice_WantSteamSend(void);
void SteamVoice_SetInGame(int inGame);
/* Force Steam receive path + install DecompressVoice hook. */
void SteamVoice_EnableSteamReceive(void);
void SteamVoice_SetSteamFlag(int on);
void SteamVoice_SetTweakMode(int on);
int SteamVoice_IsTweakMode(void);
