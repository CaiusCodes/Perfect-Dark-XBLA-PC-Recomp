// SPDX-License-Identifier: GPL-3.0-only
//
// Perfect Dark XBLA PC Recomp - Xbox 360 STFS (LIVE/PIRS/CON) package reader.
//
// Copyright (C) 2026 the Perfect Dark XBLA PC Recomp authors.
// The STFS block-offset and hash-table calculations are derived from
// Velocity (https://github.com/hetelek/Velocity), Copyright (C) the Velocity
// authors, which is licensed under the GNU General Public License version 3.
// This is a C++ port of the project's earlier Extract-STFS.ps1.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License version 3 as published by
// the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details. The full licence text is in
// packaging/licenses/LICENSE-Extract-STFS.txt (licenses\ in a release).
//
// Because this file is compiled into the launcher ("Perfect Dark.exe" in the
// release root), the launcher as a whole is distributed under the GPL-3.0.
// The game itself (resources\runtime) is a separate program and is not.
#include "stfs.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace stfs {
namespace {

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t BE24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
uint32_t LE24(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
uint32_t LE16(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8); }

// Velocity: StfsPackage::ComputeBackingDataBlockNumber
uint64_t BackingDataBlock(uint32_t block, int shift) {
  uint64_t result = (uint64_t((block + 0xAA) / 0xAA) << shift) + block;
  if (block < 0xAA) return result;
  if (block < 0x70E4) return result + (uint64_t((block + 0x70E4) / 0x70E4) << shift);
  return (uint64_t(1) << shift) + result + (uint64_t((block + 0x70E4) / 0x70E4) << shift);
}

// Velocity: StfsPackage::ComputeLevel0BackingHashBlockNumber
uint64_t Level0HashBlock(uint32_t block, int shift) {
  if (block < 0xAA) return 0;
  const uint64_t step0 = shift == 0 ? 0xAB : 0xAC;
  uint64_t num = uint64_t(block / 0xAA) * step0;
  num += uint64_t(block / 0x70E4 + 1) << shift;
  if (block / 0x70E4 == 0) return num;
  return num + (uint64_t(1) << shift);
}

// A name that is safe to create on disk: no separators, drive letters or
// "." / ".." parts, nothing Windows refuses in a file name.
bool SafeName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  for (char c : name) {
    if (c < 0x20 || c > 0x7E || std::string("<>:\"/\\|?*").find(c) != std::string::npos) return false;
  }
  return name.back() != '.' && name.back() != ' ';
}

}  // namespace

Package::~Package() { Close(); }

void Package::Close() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(mapping_);
  if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
  entries_.clear();
}

uint64_t Package::BlockToAddress(uint32_t block) const {
  return (BackingDataBlock(block, shift_) << 12) + first_hash_table_;
}

bool Package::NextBlock(uint32_t block, uint32_t* next) const {
  const uint64_t hash_addr = (Level0HashBlock(block, shift_) << 12) + first_hash_table_;
  const uint64_t record = hash_addr + uint64_t(block % 0xAA) * 0x18;
  if (record + 0x18 > size_) return false;
  const uint32_t value = BE24(data_ + record + 0x15);
  if (value >= 0xFFFFFF || value == 0xFFFFFE) return false;
  *next = value;
  return true;
}

bool Package::Open(const std::wstring& path, std::wstring* error) {
  Close();
  // Read access with read sharing only: the package is never modified.
  file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    file_ = nullptr;
    *error = L"The package could not be opened. Check that it is not in use and that you can read it.";
    return false;
  }
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0xC000) {
    *error = L"This file is too small to be an Xbox 360 package.";
    Close();
    return false;
  }
  size_ = static_cast<uint64_t>(size.QuadPart);
  mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping_) data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
  if (!data_) {
    *error = L"The package could not be read (not enough memory to map it).";
    Close();
    return false;
  }

  if (memcmp(data_, "CON ", 4) != 0 && memcmp(data_, "LIVE", 4) != 0 && memcmp(data_, "PIRS", 4) != 0) {
    *error = L"This is not an Xbox 360 package.";
    Close();
    return false;
  }
  const uint32_t header_size = BE32(data_ + 0x340);
  first_hash_table_ = (uint64_t(header_size) + 0xFFF) & 0xFFFFF000ull;
  const uint8_t block_separation = data_[0x37B];
  shift_ = (block_separation & 1) ? 0 : 1;
  const uint32_t table_blocks = LE16(data_ + 0x37C);
  uint32_t block = LE24(data_ + 0x37E);

  // The file table: 64 entries of 0x40 bytes per block.
  for (uint32_t read = 0; read < table_blocks; ++read) {
    const uint64_t at = BlockToAddress(block);
    for (int i = 0; i < 64; ++i) {
      const uint64_t entry_at = at + uint64_t(i) * 0x40;
      if (entry_at + 0x40 > size_) break;
      const uint8_t* e = data_ + entry_at;
      const int name_length = e[0x28] & 0x3F;
      if (name_length == 0) continue;
      bool printable = true;
      for (int k = 0; k < name_length; ++k) printable = printable && e[k] >= 0x20 && e[k] <= 0x7E;
      if (!printable) continue;
      Entry entry;
      entry.name.assign(reinterpret_cast<const char*>(e), name_length);
      entry.directory = (e[0x28] & 0x80) != 0;
      entry.consecutive = (e[0x28] & 0x40) != 0;
      entry.blocks = LE24(e + 0x29);
      entry.first_block = LE24(e + 0x2F);
      const uint32_t parent = (uint32_t(e[0x32]) << 8) | e[0x33];
      entry.parent = parent == 0xFFFF ? -1 : static_cast<int>(parent);
      entry.size = BE32(e + 0x34);
      if (!SafeName(entry.name)) {
        *error = L"The package's file table holds an unexpected file name. The package may be damaged.";
        Close();
        return false;
      }
      entries_.push_back(std::move(entry));
    }
    if (read + 1 < table_blocks && !NextBlock(block, &block)) break;
  }

  // Full relative paths. A parent chain longer than the table is a loop.
  for (Entry& entry : entries_) {
    std::wstring path(entry.name.begin(), entry.name.end());
    int parent = entry.parent;
    size_t guard = 0;
    while (parent >= 0 && parent < static_cast<int>(entries_.size()) && entries_[parent].directory) {
      const std::string& name = entries_[parent].name;
      path = std::wstring(name.begin(), name.end()) + L"\\" + path;
      parent = entries_[parent].parent;
      if (++guard > entries_.size()) {
        *error = L"The package's folder table is damaged.";
        Close();
        return false;
      }
    }
    entry.path = path;
  }
  if (entries_.empty()) {
    *error = L"The package's file table is empty or damaged.";
    Close();
    return false;
  }
  return true;
}

const Entry* Package::Find(const std::wstring& path) const {
  for (const Entry& entry : entries_) {
    if (!entry.directory && _wcsicmp(entry.path.c_str(), path.c_str()) == 0) return &entry;
  }
  return nullptr;
}

uint64_t Package::total_file_bytes() const {
  uint64_t total = 0;
  for (const Entry& entry : entries_) {
    if (!entry.directory) total += entry.size;
  }
  return total;
}

bool Package::Read(const Entry& entry, const std::function<bool(const uint8_t*, size_t)>& sink,
                   std::wstring* error) const {
  uint64_t remaining = entry.size;
  uint32_t block = entry.first_block;
  for (uint32_t done = 0; remaining > 0; ++done) {
    if (done >= entry.blocks) {
      *error = L"A file in the package is shorter than its size says. The package may be damaged.";
      return false;
    }
    const uint64_t at = BlockToAddress(block);
    const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, 0x1000));
    if (at + take > size_) {
      *error = L"The package ends early. It may be incomplete or damaged.";
      return false;
    }
    if (!sink(data_ + at, take)) return false;
    remaining -= take;
    if (remaining == 0) break;
    if (entry.consecutive) {
      ++block;
    } else if (!NextBlock(block, &block)) {
      *error = L"A file's block chain in the package is broken. The package may be damaged.";
      return false;
    }
  }
  return true;
}

}  // namespace stfs
