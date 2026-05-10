#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "pilots.h"

extern QueueHandle_t packetQueue;

void setupPromiscuous();
