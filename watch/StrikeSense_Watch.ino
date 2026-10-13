#include "lcd_bsp.h"
#include "FT3168.h"
#include "lora_link.h"      // NEW: LoRa link to the rod

void setup()
{
  Serial.begin(115200);
  Touch_Init();
  lcd_lvgl_Init();

  lora_link_begin();        // NEW: start the LoRa radio
}

void loop()
{
  lora_link_poll();         // NEW: receive the rod, send the buttons
  delay(2);
}
