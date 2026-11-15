#pragma once

#include <GxEPD2_BW.h>

#define EPD_MOSI   11
#define EPD_SCK    12
#define EPD_CS     10
#define EPD_DC      9
#define EPD_RST     8
#define EPD_BUSY   18
#define EPD_PWR    17

#define ENC1_CLK   38
#define ENC1_DT    39
#define ENC1_SW    40
#define ENC2_CLK   41
#define ENC2_DT    42
#define ENC2_SW    47

#if defined(RGB_BUILTIN) && (RGB_BUILTIN) != 38 && (RGB_BUILTIN) != 39 && (RGB_BUILTIN) != 40 \
    && (RGB_BUILTIN) != 41 && (RGB_BUILTIN) != 42 && (RGB_BUILTIN) != 47
  #define RGB_LED_PIN RGB_BUILTIN
#else
  #define RGB_LED_PIN 48
#endif

using Panel = GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 4>;
extern Panel display;

extern volatile int32_t enc1Steps, enc2Steps;
extern uint32_t inputEpoch;

namespace news {
  void enter();
  void service();
}
