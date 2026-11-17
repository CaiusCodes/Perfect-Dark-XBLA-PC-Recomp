// Experimental extras: see pd_assets_import.h.
#include "pd_assets_import.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <system_error>
#include <vector>

#include "pd_n64_decode.h"

namespace pd::assets {

namespace fs = std::filesystem;

namespace {

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = wchar_t(towlower(c));
  return s;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out, uint64_t max_bytes) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > max_bytes) return false;
  out.resize(size_t(size));
  std::ifstream f(path, std::ios::binary);
  return f && (size == 0 || f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)));
}

bool WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  return f && f.write(reinterpret_cast<const char*>(data), std::streamsize(size)) && f.flush();
}

bool IsNote(const fs::path& file) {
  const std::wstring ext = Lower(file.extension().wstring());
  return ext == L".txt" || ext == L".md";
}

bool IsPicture(const fs::path& file) {
  const std::wstring ext = Lower(file.extension().wstring());
  return ext == L".png";
}

// ---- .zip reading (stored and deflated entries; no ZIP64, no encryption) ----
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct ZipEntry {
  std::string name;  // as stored ('/' separated; UTF-8 when flag bit 11, else taken as is)
  uint16_t method = 0, flags = 0;
  uint32_t compressed = 0, size = 0, local_offset = 0;
};

bool ZipEntries(const std::vector<uint8_t>& zip, std::vector<ZipEntry>& out, std::string* error) {
  if (zip.size() < 22) { *error = "it is not a .zip file"; return false; }
  size_t eocd = std::string::npos;
  const size_t lowest = zip.size() > 22 + 65535 ? zip.size() - 22 - 65535 : 0;
  for (size_t i = zip.size() - 22 + 1; i-- > lowest;) {
    if (U32(&zip[i]) == 0x06054b50) { eocd = i; break; }
  }
  if (eocd == std::string::npos) { *error = "it is not a .zip file"; return false; }
  const uint16_t count = U16(&zip[eocd + 10]);
  const uint32_t cd_size = U32(&zip[eocd + 12]), cd_offset = U32(&zip[eocd + 16]);
  if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu || uint64_t(cd_offset) + cd_size > zip.size()) {
    *error = "the .zip is too large or damaged (ZIP64 isn't supported: extract it and import the folder)";
    return false;
  }
  size_t at = cd_offset;
  for (uint16_t n = 0; n < count; ++n) {
    if (at + 46 > zip.size() || U32(&zip[at]) != 0x02014b50) { *error = "the .zip is damaged"; return false; }
    ZipEntry e;
    e.flags = U16(&zip[at + 8]);
    e.method = U16(&zip[at + 10]);
    e.compressed = U32(&zip[at + 20]);
    e.size = U32(&zip[at + 24]);
    const uint16_t name_len = U16(&zip[at + 28]), extra_len = U16(&zip[at + 30]), comment_len = U16(&zip[at + 32]);
    e.local_offset = U32(&zip[at + 42]);
    if (at + 46 + name_len > zip.size()) { *error = "the .zip is damaged"; return false; }
    e.name.assign(reinterpret_cast<const char*>(&zip[at + 46]), name_len);
    out.push_back(std::move(e));
    at += 46 + size_t(name_len) + extra_len + comment_len;
  }
  return true;
}

bool ZipRead(const std::vector<uint8_t>& zip, const ZipEntry& e, std::vector<uint8_t>& out, std::string* error) {
  if (e.flags & 1) { *error = "the .zip is encrypted"; return false; }
  const size_t lh = e.local_offset;
  if (lh + 30 > zip.size() || U32(&zip[lh]) != 0x04034b50) { *error = "the .zip is damaged"; return false; }
  const size_t data = lh + 30 + U16(&zip[lh + 26]) + U16(&zip[lh + 28]);
  if (data + uint64_t(e.compressed) > zip.size()) { *error = "the .zip is damaged"; return false; }
  out.clear();
  if (e.method == 0) {
    out.assign(zip.begin() + data, zip.begin() + data + e.compressed);
  } else if (e.method == 8) {
    out.reserve(e.size);
    if (!n64dec::Inflate(&zip[data], e.compressed, out) || out.size() != e.size) {
      *error = "the .zip is damaged";
      return false;
    }
  } else {
    *error = "the .zip uses a compression this game can't read (use plain Deflate)";
    return false;
  }
  return true;
}

constexpr uint64_t kMaxPictureBytes = 64ull << 20;  // one picture
constexpr uint64_t kMaxPackBytes = 4ull << 30;      // a whole pack
constexpr int kMaxPictures = 20000;

}  // namespace

int PackFileId(const fs::path& file) {
  if (!IsPicture(file)) return -1;
  const std::wstring stem = file.stem().wstring();
  int id = 0;
  size_t digits = 0;
  while (digits < stem.size() && stem[digits] >= L'0' && stem[digits] <= L'9') {
    id = id * 10 + int(stem[digits] - L'0');
    if (++digits > 5) return -1;
  }
  if (!digits || id > kMaxTextureId) return -1;
  return id;
}

std::string Md5Hex(const uint8_t* data, size_t size) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  uint8_t digest[16] = {};
  std::string out;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0) {
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
      if (BCryptHashData(hash, const_cast<PUCHAR>(data), ULONG(size), 0) == 0 &&
          BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (uint8_t b : digest) {
          out += kHex[b >> 4];
          out += kHex[b & 15];
        }
      }
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return out;
}

bool ImportN64Rom(const fs::path& src, const fs::path& dest_folder, std::string* version, std::string* message) {
  std::vector<uint8_t> rom;
  std::error_code ec;
  const uint64_t size = fs::file_size(src, ec);
  if (ec) { *message = "Can't read " + Utf8(src.filename().wstring()) + "."; return false; }
  if (size != 0x2000000) {
    *message = Utf8(src.filename().wstring()) + " isn't an N64 Perfect Dark ROM (those are 32 MB).";
    return false;
  }
  if (!ReadFile(src, rom, 0x2000000)) { *message = "Can't read " + Utf8(src.filename().wstring()) + "."; return false; }
  if (!n64dec::NormaliseRomByteOrder(rom)) {
    *message = Utf8(src.filename().wstring()) + " isn't an N64 ROM.";
    return false;
  }
  const std::string md5 = Md5Hex(rom.data(), rom.size());
  const n64dec::RomVersion* v = n64dec::FindRomVersion(md5);
  if (!v) {
    *message = Utf8(src.filename().wstring()) +
               " isn't a Perfect Dark ROM this game knows (it takes the NTSC 1.0, NTSC and PAL versions).";
    return false;
  }
  if (!v->texture_seg) {
    *message = "N64 textures from the Japanese Perfect Dark ROM aren't supported yet.";
    return false;
  }
  fs::create_directories(dest_folder, ec);
  const fs::path tmp = dest_folder / "pd.z64.importing";
  if (!WriteFile(tmp, rom.data(), rom.size())) {
    fs::remove(tmp, ec);
    *message = "Can't write to " + Utf8(dest_folder.wstring()) + ".";
    return false;
  }
  // Only one ROM in the folder: the game takes the first by name.
  for (const auto& e : fs::directory_iterator(dest_folder, ec)) {
    const std::wstring ext = Lower(e.path().extension().wstring());
    if (e.is_regular_file(ec) && (ext == L".z64" || ext == L".n64" || ext == L".v64")) fs::remove(e.path(), ec);
  }
  fs::rename(tmp, dest_folder / "pd.z64", ec);
  if (ec) {
    fs::remove(tmp, ec);
    *message = "Can't write to " + Utf8(dest_folder.wstring()) + ".";
    return false;
  }
  if (version) *version = v->name;
  *message = std::string("N64 ROM added (Perfect Dark ") + v->name + ").";
  return true;
}

std::string SafePackName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')') out += char(c);
    else if (c >= 0x80) continue;  // keep pack names plain ASCII: the game's font has no others
    else out += '_';
    if (out.size() >= 40) break;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  return out;
}

bool ImportTexturePack(const fs::path& src, const fs::path& packs_folder, std::string* name, int* pictures,
                       std::string* message) {
  std::error_code ec;
  const bool is_folder = fs::is_directory(src, ec);
  const bool is_zip = !is_folder && Lower(src.extension().wstring()) == L".zip";
  if (!is_folder && !is_zip) {
    *message = "Choose a texture pack's .zip, or a picture inside its folder.";
    return false;
  }
  const std::string pack = SafePackName(Utf8((is_zip ? src.stem() : src.filename()).wstring()));
  if (pack.empty()) { *message = "The pack needs a name made of letters or digits."; return false; }
  const fs::path dest = packs_folder / Wide(pack);
  if (is_folder && fs::equivalent(src, dest, ec)) {
    // A pack already in the packs folder (a picture in it was picked): nothing to copy.
    int count = 0;
    for (auto it = fs::recursive_directory_iterator(dest, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
      if (it->is_regular_file(ec) && PackFileId(it->path()) >= 0) ++count;
    if (name) *name = pack;
    if (pictures) *pictures = count;
    *message = "Texture pack \"" + pack + "\" is already installed (" + std::to_string(count) + " pictures).";
    return true;
  }
  fs::create_directories(packs_folder, ec);
  const fs::path staging = packs_folder / Wide(".importing-" + pack);
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) { *message = "Can't write to " + Utf8(packs_folder.wstring()) + "."; return false; }
  int count = 0;
  uint64_t total = 0;
  std::string error;
  // Pictures are stored flat, by id (two files for one id: the first by name wins).
  auto keep = [&](const fs::path& file_name, const uint8_t* data, size_t size) -> bool {
    if (count >= kMaxPictures || (total += size) > kMaxPackBytes) {
      error = "the pack is too large";
      return false;
    }
    const fs::path out = staging / file_name.filename();
    if (fs::exists(out, ec)) return true;
    if (!WriteFile(out, data, size)) {
      error = "can't write to " + Utf8(packs_folder.wstring());
      return false;
    }
    if (PackFileId(file_name) >= 0) ++count;
    return true;
  };
  bool ok = true;
  if (is_folder) {
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const bool top = it.depth() == 0;
      if (PackFileId(it->path()) >= 0 || (top && IsNote(it->path()))) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
      std::vector<uint8_t> bytes;
      if (!ReadFile(f, bytes, kMaxPictureBytes)) continue;  // too large or unreadable: left out
      if (!(ok = keep(f, bytes.data(), bytes.size()))) break;
    }
  } else {
    std::vector<uint8_t> zip;
    if (!ReadFile(src, zip, kMaxPackBytes)) {
      error = "can't read the .zip (or it is over 4 GB)";
      ok = false;
    }
    std::vector<ZipEntry> entries;
    if (ok && !ZipEntries(zip, entries, &error)) ok = false;
    if (ok) {
      std::sort(entries.begin(), entries.end(), [](const ZipEntry& a, const ZipEntry& b) { return a.name < b.name; });
      for (const ZipEntry& e : entries) {
        if (e.name.empty() || e.name.back() == '/') continue;
        // Only the file name counts: no folders are created from the .zip.
        const size_t slash = e.name.find_last_of("/\\");
        const fs::path file = Wide(slash == std::string::npos ? e.name : e.name.substr(slash + 1));
        const bool top = e.name.find_first_of("/\\") == std::string::npos ||
                         e.name.find_first_of("/\\") == e.name.find_last_of("/\\");
        if (!(PackFileId(file) >= 0 || (top && IsNote(file)))) continue;
        if (e.size > kMaxPictureBytes) continue;
        std::vector<uint8_t> bytes;
        if (!ZipRead(zip, e, bytes, &error)) { ok = false; break; }
        if (!(ok = keep(file, bytes.data(), bytes.size()))) break;
      }
    }
  }
  if (ok && count == 0) {
    error = "it has no pictures named by texture number (like 0123.png)";
    ok = false;
  }
  if (!ok) {
    fs::remove_all(staging, ec);
    *message = "Texture pack not added: " + error + ".";
    return false;
  }
  fs::remove_all(dest, ec);
  fs::rename(staging, dest, ec);
  if (ec) {
    fs::remove_all(staging, ec);
    *message = "Can't write to " + Utf8(packs_folder.wstring()) + ".";
    return false;
  }
  if (name) *name = pack;
  if (pictures) *pictures = count;
  *message = "Texture pack \"" + pack + "\" added (" + std::to_string(count) + " pictures).";
  return true;
}

}  // namespace pd::assets
