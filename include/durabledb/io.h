#pragma once
// Thin wrappers over the POSIX calls the engine needs.
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace durabledb::io {

// write() may write less than asked, so loop until everything is written.
inline bool WriteAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// pread() until n bytes are read or an error / EOF happens.
inline bool ReadAt(int fd, void* buf, size_t n, off_t off) {
  char* p = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t r = ::pread(fd, p, n, off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;  // unexpected EOF
    p += r;
    n -= static_cast<size_t>(r);
    off += r;
  }
  return true;
}

inline std::string DirName(const std::string& path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

// A rename() only survives a crash once the directory itself is fsynced.
inline bool FsyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

inline bool ReadFile(const std::string& path, std::string* out) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  off_t size = ::lseek(fd, 0, SEEK_END);
  if (size < 0) {
    ::close(fd);
    return false;
  }
  out->assign(static_cast<size_t>(size), '\0');
  bool ok = size == 0 || ReadAt(fd, out->data(), out->size(), 0);
  ::close(fd);
  return ok;
}

// Crash-safe file replace: write path.tmp, fsync it, rename over path, fsync dir.
// A reader sees either the old file or the complete new file, never a mix.
inline bool WriteFileAtomic(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return false;
  bool ok = WriteAll(fd, data.data(), data.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
  return FsyncDir(DirName(path));
}

}  // namespace durabledb::io
