#pragma once
#include <Arduino.h>

void wsText(const String& msg);
void wsText(const char* msg);
void initWsHandler();
