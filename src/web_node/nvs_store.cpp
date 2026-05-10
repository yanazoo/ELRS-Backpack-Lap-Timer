#include <Arduino.h>
#include "nvs_store.h"
#include "data_model.h"

PilotRoster roster[MAX_REGISTERED];
CalibConfig rosterCal[MAX_REGISTERED];
int         rosterCount   = 0;
int         activePilots[MAX_ACTIVE];
Preferences prefs;

void saveRosterPilot(int i) {
    if (i < 0 || i >= rosterCount) return;
    prefs.begin("pilots", false);
    char key[24];
    snprintf(key, sizeof(key), "p%d_name",  i); prefs.putString(key, roster[i].name);
    snprintf(key, sizeof(key), "p%d_yomi",  i); prefs.putString(key, roster[i].yomi);
    snprintf(key, sizeof(key), "p%d_uid",   i);
    if (roster[i].hasUid) {
        char u[18]; uidToStr(roster[i].uid, u); prefs.putString(key, u);
    } else { prefs.putString(key, ""); }
    snprintf(key, sizeof(key), "p%d_enter", i); prefs.putInt(key, rosterCal[i].enterRssi);
    snprintf(key, sizeof(key), "p%d_exit",  i); prefs.putInt(key, rosterCal[i].exitRssi);
    prefs.putInt("count", rosterCount);
    prefs.putInt("ver", 3);
    prefs.end();
}

void saveRosterCount() {
    prefs.begin("pilots", false);
    prefs.putInt("count", rosterCount);
    prefs.putInt("ver", 3);
    prefs.end();
}

void loadRosterConfig() {
    prefs.begin("pilots", true);
    rosterCount = prefs.getInt("count", 0);
    if (rosterCount > MAX_REGISTERED) rosterCount = MAX_REGISTERED;
    for (int i = 0; i < rosterCount; i++) {
        char key[24];
        snprintf(key, sizeof(key), "p%d_name", i);
        String n = prefs.getString(key, "");
        if (n.length()) strncpy(roster[i].name, n.c_str(), sizeof(roster[i].name)-1);
        else            snprintf(roster[i].name, sizeof(roster[i].name), "Pilot %d", i+1);

        snprintf(key, sizeof(key), "p%d_yomi", i);
        String y = prefs.getString(key, "");
        strncpy(roster[i].yomi, y.c_str(), sizeof(roster[i].yomi)-1);

        snprintf(key, sizeof(key), "p%d_uid", i);
        String u = prefs.getString(key, "");
        roster[i].hasUid = (u.length() == 17);
        if (roster[i].hasUid)
            sscanf(u.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                   &roster[i].uid[0], &roster[i].uid[1], &roster[i].uid[2],
                   &roster[i].uid[3], &roster[i].uid[4], &roster[i].uid[5]);

        snprintf(key, sizeof(key), "p%d_enter", i);
        rosterCal[i].enterRssi = prefs.getInt(key, DEFAULT_ENTER);
        snprintf(key, sizeof(key), "p%d_exit", i);
        rosterCal[i].exitRssi  = prefs.getInt(key, DEFAULT_EXIT);
    }
    prefs.end();
}

void saveActive() {
    prefs.begin("active", false);
    for (int s = 0; s < MAX_ACTIVE; s++) {
        char key[4]; snprintf(key, sizeof(key), "a%d", s);
        prefs.putInt(key, activePilots[s]);
    }
    prefs.end();
}

void loadActiveConfig() {
    prefs.begin("active", true);
    for (int s = 0; s < MAX_ACTIVE; s++) {
        char key[4]; snprintf(key, sizeof(key), "a%d", s);
        int ri = prefs.getInt(key, -1);
        activePilots[s] = (ri >= 0 && ri < rosterCount) ? ri : -1;
    }
    prefs.end();
}

// Rewrites NVS for all pilots from fromId onward, then clears the old last slot.
// Called after a pilot is deleted and the roster array has already been shifted.
void nvsSaveRangeFromIndex(int fromId) {
    prefs.begin("pilots", false);
    prefs.putInt("count", rosterCount);
    prefs.putInt("ver", 3);
    for (int i = fromId; i < rosterCount; i++) {
        char key[24];
        snprintf(key, sizeof(key), "p%d_name",  i); prefs.putString(key, roster[i].name);
        snprintf(key, sizeof(key), "p%d_yomi",  i); prefs.putString(key, roster[i].yomi);
        snprintf(key, sizeof(key), "p%d_uid",   i);
        if (roster[i].hasUid) {
            char u[18]; uidToStr(roster[i].uid, u); prefs.putString(key, u);
        } else { prefs.putString(key, ""); }
        snprintf(key, sizeof(key), "p%d_enter", i); prefs.putInt(key, rosterCal[i].enterRssi);
        snprintf(key, sizeof(key), "p%d_exit",  i); prefs.putInt(key, rosterCal[i].exitRssi);
    }
    // Clear the NVS entry for the slot that no longer exists
    char key[24];
    snprintf(key, sizeof(key), "p%d_name",  rosterCount); prefs.remove(key);
    snprintf(key, sizeof(key), "p%d_yomi",  rosterCount); prefs.remove(key);
    snprintf(key, sizeof(key), "p%d_uid",   rosterCount); prefs.remove(key);
    snprintf(key, sizeof(key), "p%d_enter", rosterCount); prefs.remove(key);
    snprintf(key, sizeof(key), "p%d_exit",  rosterCount); prefs.remove(key);
    prefs.end();
}
