#pragma once
#include <Arduino.h>

void sendLap(int idx);
void sendRssi(int idx, uint32_t now);
void processWebCmd(const String& line);
