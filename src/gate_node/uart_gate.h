#pragma once
#include <Arduino.h>

extern uint32_t gCooldownMs;

void sendLap(int idx, uint32_t lapMs);
void sendRssi(int idx, uint32_t now);
void processWebCmd(const String& line);
