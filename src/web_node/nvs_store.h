#pragma once

void loadRosterConfig();
void loadActiveConfig();
void saveRosterPilot(int i);
void saveRosterCount();
void saveActive();
void nvsSaveRangeFromIndex(int fromId);  // rewrite all pilots at fromId..rosterCount-1 + clear last
