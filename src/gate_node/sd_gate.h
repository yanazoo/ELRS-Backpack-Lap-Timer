#pragma once
#include <stdint.h>

extern bool    sdPresent;
extern bool    sdPollEnabled;
extern uint8_t sdLogMode;   // 0=always (new file each race), 1=rotate (auto-delete oldest), 2=off

void sdInit();
void sdSendStatus();
void sdCheckHotplug(uint32_t now);
void sdBeginRace();
void sdWriteLap(int slotIdx, uint32_t lapMs, int lapCount);
void sdEndRace();
void sdBeginBackup();
void sdWriteBackupRow(const char* name, const char* yomi,
                      const char* mac, int enter, int exit_, int slot);
void sdEndBackup();
void sdHandleRestore();
void sdListFiles();
void sdReadFile(const char* path);
void sdDeleteFile(const char* path);
