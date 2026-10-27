#pragma once
#include <map>
#include <string>
#include <iterator>
struct Preferences {
  // like the ESP32: each namespace has its own keys
  std::string ns = "";
  std::string key(const char* k){ return ns + "/" + k; }
  static std::map<std::string,float>& f(){ static std::map<std::string,float> m; return m; }
  bool begin(const char* n, bool){ ns = n; return true;} void end(){}
  bool getBool(const char* k,bool d){ return f().count(key(k))? f()[key(k)]!=0 : d; }
  float getFloat(const char* k,float d){ return f().count(key(k))? f()[key(k)] : d; }
  unsigned long getULong(const char* k,unsigned long d){ return f().count(key(k))? (unsigned long)f()[key(k)] : d; } void putULong(const char* k,unsigned long v){ f()[key(k)]=v; }
  void putBool(const char* k,bool v){ f()[key(k)]=v; } void putFloat(const char* k,float v){ f()[key(k)]=v; }
  int getInt(const char* k,int d){ return f().count(key(k))? (int)f()[key(k)] : d; } void putInt(const char* k,int v){ f()[key(k)]=v; }
  static std::map<std::string,std::string>& s(){ static std::map<std::string,std::string> m; return m; }
  String getString(const char* k,String d){ return s().count(key(k))? String(s()[key(k)].c_str()) : d; }
  void putString(const char* k,String v){ s()[key(k)]=v.c_str(); }
  void remove(const char* k){ f().erase(key(k)); s().erase(key(k)); }
  void clear(){
    std::string p = ns + "/";
    for(auto it = f().begin(); it != f().end(); ) it = it->first.compare(0, p.size(), p) == 0 ? f().erase(it) : std::next(it);
    for(auto it = s().begin(); it != s().end(); ) it = it->first.compare(0, p.size(), p) == 0 ? s().erase(it) : std::next(it);
  }
};
