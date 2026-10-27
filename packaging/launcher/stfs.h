// SPDX-License-Identifier: GPL-3.0-only
//
// Perfect Dark XBLA PC Recomp - Xbox 360 STFS (LIVE/PIRS/CON) package reader.
// See stfs.cpp for the licence notice.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stfs {

struct Entry {
  std::string name;
  std::wstring path;  // relative path inside the package, backslash separated
  bool directory = false;
  bool consecutive = false;
  uint32_t blocks = 0;
  uint32_t first_block = 0;
  int parent = -1;
  uint32_t size = 0;
};

// A package opened read-only (memory mapped). The file is never written.
class Package {
 public:
  Package() = default;
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;

  // Maps the file and reads its file table. On failure returns false and sets
  // *error to a sentence for the player.
  bool Open(const std::wstring& path, std::wstring* error);
  void Close();

  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* Find(const std::wstring& path) const;  // case-insensitive
  uint64_t total_file_bytes() const;

  // Streams a file's data to sink in pieces of at most 4 KB. Stops (and
  // returns false) when sink returns false or the block chain is damaged.
  bool Read(const Entry& entry, const std::function<bool(const uint8_t*, size_t)>& sink,
            std::wstring* error) const;

 private:
  uint64_t BlockToAddress(uint32_t block) const;
  bool NextBlock(uint32_t block, uint32_t* next) const;

  void* file_ = nullptr;
  void* mapping_ = nullptr;
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
  int shift_ = 0;
  uint64_t first_hash_table_ = 0;
  std::vector<Entry> entries_;
};

}  // namespace stfs
