#pragma once
#include "Arduino.h"
struct WiFiT { void softAP(const char*,const char*){} const char* softAPIP(){ return "192.168.4.1"; } };
extern WiFiT WiFi;
