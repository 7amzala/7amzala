#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
using std::sqrt; using std::fabs; using std::acos; using std::atan2;
#define PROGMEM
extern unsigned long simMillis;
inline unsigned long millis(){ return simMillis; }
inline unsigned long micros(){ return simMillis * 1000UL; }
#define HEX 16
inline void delay(unsigned long){}
inline long random(long a, long b){ return a + rand() % (b - a); }
template<class T> inline T constrain(T v, T lo, T hi){ return v < lo ? lo : (v > hi ? hi : v); }
#include <algorithm>
using std::min;
using std::max;
class String {
public:
  std::string s;
  String(){} String(const char* c):s(c){} String(const std::string& x):s(x){}
  String(long v){ s=std::to_string(v);} String(int v){ s=std::to_string(v);} String(unsigned long v){ s=std::to_string(v);}
  String(float v, int d){ char b[64]; snprintf(b,64,"%.*f",d,v); s=b; }
  String(double v, int d){ char b[64]; snprintf(b,64,"%.*f",d,v); s=b; }
  void reserve(size_t n){ s.reserve(n);} 
  String& operator+=(const String& o){ s+=o.s; return *this;}
  String& operator+=(const char* o){ s+=o; return *this;}
  bool operator==(const char* o) const { return s==o; }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  const char* c_str() const { return s.c_str(); }
  float toFloat() const { return atof(s.c_str()); }
  // used by the Bluetooth link (BLE test)
  size_t length() const { return s.size(); }
  char operator[](size_t i) const { return i < s.size() ? s[i] : 0; }
  int indexOf(char c) const { size_t p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  String substring(size_t a) const { return a < s.size() ? String(s.substr(a)) : String(); }
  String substring(size_t a, size_t b) const { return a < s.size() ? String(s.substr(a, b - a)) : String(); }
  long toInt() const { return atol(s.c_str()); }
  bool operator==(const String& o) const { return s == o.s; }
  String& operator+=(char c){ s += c; return *this; }
  bool concat(const char* p, unsigned n){ s.append(p, n); return true; }
  void trim(){ size_t a = s.find_first_not_of(" \t\r\n"); if(a == std::string::npos){ s.clear(); return; } size_t b = s.find_last_not_of(" \t\r\n"); s = s.substr(a, b - a + 1); }
  friend String operator+(const String& a, const String& b){ return String(a.s + b.s); }
  friend String operator+(const char* a, const String& b){ return String(std::string(a) + b.s); }
  friend String operator+(const String& a, const char* b){ return String(a.s + b); }
};
struct SerialT {
  bool quiet=false;
  template<class T> void print(T v){ if(!quiet) out(v); }
  template<class T> void println(T v){ if(!quiet){ out(v); printf("\n");} }
  void print(float v,int d){ if(!quiet) printf("%.*f",d,v);} void print(uint8_t v,int b){ if(!quiet) printf(b==16?"%X":"%d",v);} void println(float v,int d){ if(!quiet) printf("%.*f\n",d,v);} 
  void println(){ if(!quiet) printf("\n"); }
  void begin(int){}
  template<class... A> void printf(const char* f, A... a){ if(!quiet) ::printf(f, a...); }
  void out(const char* c){ printf("[%7.1fs] %s", simMillis/1000.0, c);} void out(const String& s){ printf("%s", s.c_str()); }
  void out(int v){ printf("%d",v);} void out(long v){printf("%ld",v);} void out(unsigned long v){printf("%lu",v);} void out(float v){printf("%.3f",v);} void out(double v){printf("%.3f",v);}
};
extern SerialT Serial;
// GPIO (no real pins on the PC: reads LOW)
#define LOW 0
#define HIGH 1
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return LOW; }
#define D1 2
inline unsigned analogReadMilliVolts(int) { return 1950; }   // 3.90 V battery
inline void delayMicroseconds(unsigned) {}
// chip ID and random numbers (rod ID + PIN)
#define ESP_MAC_BT 2
inline int esp_read_mac(uint8_t* m, int) { const uint8_t mac[6] = { 0x34, 0x85, 0x18, 0x00, 0x4F, 0x2A }; for(int i = 0; i < 6; i++) m[i] = mac[i]; return 0; }
inline uint32_t esp_random() { return (uint32_t)rand() * 2654435761u; }
