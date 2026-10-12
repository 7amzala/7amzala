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
  const char* c_str() const { return s.c_str(); }
  float toFloat() const { return atof(s.c_str()); }
};
struct SerialT {
  bool quiet=false;
  template<class T> void print(T v){ if(!quiet) out(v); }
  template<class T> void println(T v){ if(!quiet){ out(v); printf("\n");} }
  void print(float v,int d){ if(!quiet) printf("%.*f",d,v);} void print(uint8_t v,int b){ if(!quiet) printf(b==16?"%X":"%d",v);} void println(float v,int d){ if(!quiet) printf("%.*f\n",d,v);} 
  void println(){ if(!quiet) printf("\n"); }
  void begin(int){}
  void out(const char* c){ printf("[%7.1fs] %s", simMillis/1000.0, c);} void out(const String& s){ printf("%s", s.c_str()); }
  void out(int v){ printf("%d",v);} void out(long v){printf("%ld",v);} void out(unsigned long v){printf("%lu",v);} void out(float v){printf("%.3f",v);} void out(double v){printf("%.3f",v);}
};
extern SerialT Serial;
