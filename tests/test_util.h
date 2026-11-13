#pragma once
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Creates a fresh temp directory and deletes it at the end of the test.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/durabledb_test_XXXXXX";
    path_ = mkdtemp(tmpl);
  }
  ~TempDir() {
    std::string cmd = "rm -rf '" + path_ + "'";
    if (std::system(cmd.c_str()) != 0) {}
  }
  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

inline std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

inline void WriteAll(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

inline bool Exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

inline std::vector<std::string> ListDir(const std::string& dir) {
  std::vector<std::string> names;
  std::string cmd = "ls -1 '" + dir + "' > '" + dir + "/.ls' 2>/dev/null";
  if (std::system(cmd.c_str()) != 0) return names;
  std::ifstream in(dir + "/.ls");
  std::string line;
  while (std::getline(in, line)) names.push_back(line);
  ::unlink((dir + "/.ls").c_str());
  return names;
}
