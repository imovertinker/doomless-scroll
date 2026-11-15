// Doomless Scroll: read articles on a 7.5" e-paper display with two knobs.
// This file sets up the display and the rotary encoders.
// Everything about fetching and drawing articles is in news.cpp.

#include <SPI.h>
#include <driver/gpio.h>

#include "fonts.h"
#include "hw.h"

Panel display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

volatile uint8_t enc1Ab = 0, enc2Ab = 0;
volatile int32_t enc1Steps = 0, enc2Steps = 0;
uint32_t inputEpoch = 0;

static const DRAM_ATTR int8_t kQuad[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};

void IRAM_ATTR enc1Isr()
{
  uint8_t curr = (gpio_get_level((gpio_num_t)ENC1_CLK) << 1) | gpio_get_level((gpio_num_t)ENC1_DT);
  enc1Steps += kQuad[(enc1Ab << 2) | curr];
  enc1Ab = curr;
}

void IRAM_ATTR enc2Isr()
{
  uint8_t curr = (gpio_get_level((gpio_num_t)ENC2_CLK) << 1) | gpio_get_level((gpio_num_t)ENC2_DT);
  enc2Steps += kQuad[(enc2Ab << 2) | curr];
  enc2Ab = curr;
}

void setup()
{
  Serial.begin(115200);
  delay(1200);
  Serial.println("\nDoomless Scroll");

  pinMode(ENC1_CLK, INPUT_PULLUP);
  pinMode(ENC1_DT,  INPUT_PULLUP);
  pinMode(ENC1_SW,  INPUT_PULLUP);
  pinMode(ENC2_CLK, INPUT_PULLUP);
  pinMode(ENC2_DT,  INPUT_PULLUP);
  pinMode(ENC2_SW,  INPUT_PULLUP);
  enc1Ab = (digitalRead(ENC1_CLK) << 1) | digitalRead(ENC1_DT);
  enc2Ab = (digitalRead(ENC2_CLK) << 1) | digitalRead(ENC2_DT);
  attachInterrupt(digitalPinToInterrupt(ENC1_CLK), enc1Isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC1_DT),  enc1Isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC2_CLK), enc2Isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC2_DT),  enc2Isr, CHANGE);

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  rgbLedWrite(RGB_LED_PIN, 0, 0, 0);

  delay(100);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(false);

  news::enter();
}

void loop()
{
  news::service();
}
