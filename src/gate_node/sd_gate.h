#pragma once

extern bool sdPresent;

void sdInit();
void sdSendStatus();
void sdBeginRace();
void sdWriteLap(int slotIdx, uint32_t lapMs);
void sdEndRace();
void sdBeginBackup();
void sdWriteBackupRow(const char* name, const char* yomi,
                      const char* mac, int enter, int exit_);
void sdEndBackup();
void sdHandleRestore();
void sdListFiles();
void sdReadFile(const char* path);
void sdDeleteFile(const char* path);
