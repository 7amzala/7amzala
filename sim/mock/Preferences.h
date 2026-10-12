#pragma once
#include <map>
#include <string>
struct Preferences {
  static std::map<std::string,float>& f(){ static std::map<std::string,float> m; return m; }
  bool begin(const char*, bool){ return true;} void end(){}
  bool getBool(const char* k,bool d){ return f().count(k)? f()[k]!=0 : d; }
  float getFloat(const char* k,float d){ return f().count(k)? f()[k] : d; }
  unsigned long getULong(const char* k,unsigned long d){ return f().count(k)? (unsigned long)f()[k] : d; } void putULong(const char* k,unsigned long v){ f()[k]=v; }
  void putBool(const char* k,bool v){ f()[k]=v; } void putFloat(const char* k,float v){ f()[k]=v; }
};
