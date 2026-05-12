#pragma once
#include <Arduino.h>

void sendGateCmd(const char* action);
void sendGateCooldown();
void sendGatePilot(int slot);
void sendGateThreshold(int slot);
void sendAllPilots();
void sendAllThresholds();
void processGateLine(const String& line);
void updateScanMac(const char* mac, int rssi);
