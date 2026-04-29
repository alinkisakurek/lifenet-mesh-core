#pragma once
#include <Arduino.h>

static const uint8_t  TX_BUF_SLOTS   = 8;
static const uint8_t  TX_MSG_MAX_LEN = 64;

struct TxRingBuffer {
  char    data[TX_BUF_SLOTS][TX_MSG_MAX_LEN];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
};
