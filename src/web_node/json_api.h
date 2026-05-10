#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>

String rosterJson();
String activeJson();
String lapsJson();
String scanJson();

const char* activeName(int slot);
int         activeSlotOf(int ri);

void handleBody(AsyncWebServerRequest* req,
                uint8_t* data, size_t len, size_t index, size_t total,
                std::function<void(AsyncWebServerRequest*, const char*)> cb);
