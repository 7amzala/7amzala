// =====================================================
// StrikeSense WATCH - main file
// ESP32-S3-Touch-AMOLED-1.43 + SX1262 LoRa Node (HF)
//
// The screens are in lcd_bsp.c, the radio in lora_link.cpp.
// Arduino IDE: Tools -> USB CDC On Boot -> Enabled
// =====================================================

#include "lcd_bsp.h"
#include "FT3168.h"
#include "lora_link.h"

void setup()
{
  Serial.begin(115200);

  // Touch + screen (same as the Waveshare example).
  // If your old .ino used other names here, keep YOUR lines.
  Touch_Init();
  lcd_lvgl_Init();

  // LoRa link to the rod
  lora_link_begin();
}

void loop()
{
  // Keeps the radio running: receives the rod, sends the buttons
  lora_link_poll();
  delay(2);
}
