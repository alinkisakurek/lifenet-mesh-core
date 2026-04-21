#pragma once
#include <Arduino.h>
#define DEBUG_ENABLED 1

#if DEBUG_ENABLED
#  define DBG_PRINT(x)      Serial.print(x)
#  define DBG_PRINTLN(x)    Serial.println(x)
#  define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
#  define DBG_PRINT(x)
#  define DBG_PRINTLN(x)
#  define DBG_PRINTF(...)
#endif
