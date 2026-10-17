#pragma once
#include "Arduino.h"
#include <map>
struct WebServer {
  String lastBody; String argVal;
  WebServer(int){}
  void on(const char*, void(*)()){}
  void begin(){} void handleClient(){}
  void send(int, const char*, const String& b){ lastBody=b; }
  void send(int, const char*, const char* b){ lastBody=String(b); }
  void send_P(int, const char*, const char* b){ lastBody=String(b); }
  void send_P(int, const char*, const char*, size_t n){ lastBody=String("(binary)"); (void)n; }
  void sendHeader(const char*, const char*){}
  String arg(const char*){ return argVal; }
};
