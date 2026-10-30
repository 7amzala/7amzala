#pragma once
// Host stand-in for the ESP32's LittleFS: files kept in memory. A test can
// "switch the rod off" (the files stay) or remove the storage (begin fails).
#include <map>
#include <string>
#include <cstring>

struct MockFs
{
  std::map<std::string, std::string> files;
  bool works = true;
  int writes = 0;                       // strike log files written (wear check)
  int allWrites = 0;                    // every file written
  size_t total = 1536 * 1024;           // storage size
};

inline MockFs& mockFs() { static MockFs m; return m; }

class File
{
public:
  File() {}
  File(const std::string& p, bool w) : path(p), ok(true), writing(w) {}
  explicit operator bool() const { return ok; }
  size_t read(uint8_t* b, size_t n)
  {
    std::string& d = mockFs().files[path];
    size_t k = pos < d.size() ? std::min(n, d.size() - pos) : 0;
    memcpy(b, d.data() + pos, k);
    pos += k;
    return k;
  }
  size_t write(const uint8_t* b, size_t n)
  {
    mockFs().files[path].append((const char*)b, n);
    return n;
  }
  size_t size() { return mockFs().files[path].size(); }
  void close()
  {
    if(ok && writing)
    {
      mockFs().allWrites++;
      if(path.rfind("/sl/", 0) == 0) mockFs().writes++;
    }
    ok = false;
  }
private:
  std::string path;
  bool ok = false;
  bool writing = false;
  size_t pos = 0;
};

struct LittleFSClass
{
  bool begin(bool = false) { return mockFs().works; }
  size_t totalBytes() { return mockFs().total; }
  bool mkdir(const char*) { return true; }
  bool exists(const char* p) { return mockFs().files.count(p) > 0; }
  File open(const char* p, const char* mode)
  {
    bool w = mode[0] == 'w';
    if(!w && !exists(p)) return File();
    if(w) mockFs().files[p] = "";
    return File(p, w);
  }
};

inline LittleFSClass LittleFS;
