// Experimental extras: see pd_assets_import.h.
#include "pd_assets_import.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
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

int HexDigit(wchar_t c) {
  return c >= L'0' && c <= L'9' ? int(c - L'0')
         : c >= L'a' && c <= L'f' ? int(c - L'a' + 10)
         : c >= L'A' && c <= L'F' ? int(c - L'A' + 10)
                                  : -1;
}

bool IsXblaFolder(const fs::path& name) { return _wcsicmp(name.c_str(), L"xbla") == 0; }

// Where a file sits inside a pack (`rel`: its path inside the pack folder).
enum class Place { kTop, kXbla, kOther };
Place PlaceOf(const fs::path& rel) {
  const fs::path dir = rel.parent_path();
  if (dir.empty()) return Place::kTop;
  if (dir.parent_path().empty() && IsXblaFolder(dir)) return Place::kXbla;
  return Place::kOther;
}

// The naming rule (pd_assets_import.h): an xbla folder, or a top-folder picture
// with a four-hex-digit name that has a letter in it.
bool DecidePcPort(const std::vector<fs::path>& rels, bool has_xbla_folder) {
  if (has_xbla_folder) return true;
  for (const fs::path& rel : rels) {
    const Place place = PlaceOf(rel);
    if (place == Place::kXbla && IsPicture(rel)) return true;
    if (place != Place::kTop || HexPackFileId(rel) < 0) continue;
    for (wchar_t c : rel.stem().wstring().substr(0, 4))  // (not a -xbla suffix's letters)
      if (HexDigit(c) >= 10) return true;
  }
  return false;
}

// The pack's own folder inside what was given (a folder's or a .zip's files, by
// path inside it): the folder that directly holds the most pictures named by id, an
// xbla folder counting for the folder it is in; the shallowest on a tie. Release
// .zip files put a README beside the pack's folder, and the PC port's Quest packs
// keep their pictures in an ext_tex folder inside it.
fs::path FindPackRoot(const std::vector<fs::path>& rels) {
  std::map<fs::path, int> counts;
  for (const fs::path& rel : rels) {
    if (!IsIdPicture(rel)) continue;
    fs::path dir = rel.parent_path();
    if (!dir.empty() && IsXblaFolder(dir.filename())) dir = dir.parent_path();
    ++counts[dir];
  }
  fs::path best;
  int best_n = 0;
  size_t best_depth = SIZE_MAX;
  for (const auto& [dir, n] : counts) {
    const size_t depth = size_t(std::distance(dir.begin(), dir.end()));
    if (n > best_n || (n == best_n && depth < best_depth)) {
      best = dir;
      best_n = n;
      best_depth = depth;
    }
  }
  return best;
}

// `rel` relative to the pack root `root`, or an empty path if it isn't inside it.
fs::path UnderRoot(const fs::path& rel, const fs::path& root) {
  if (root.empty()) return rel;
  const fs::path r = rel.lexically_relative(root);
  if (r.empty() || *r.begin() == L"..") return {};
  return r;
}

// A PC-port per-model picture: `in` (relative to the pack root) is
// "<model file name>\<1-6 hex digits>.<png|jpg>", the folder not xbla nor a font one.
bool ParseModelPicture(const fs::path& in, std::string* file, uint32_t* offset) {
  const fs::path dir = in.parent_path();
  if (dir.empty() || !dir.parent_path().empty() || IsXblaFolder(dir) || IsPcPortFontFolder(dir) || !IsPicture(in))
    return false;
  const std::wstring stem = in.stem().wstring();
  if (stem.empty() || stem.size() > 6) return false;
  uint32_t v = 0;
  for (wchar_t c : stem) {
    const int d = HexDigit(c);
    if (d < 0) return false;
    v = v * 16 + uint32_t(d);
  }
  if (file) *file = Utf8(dir.wstring());
  if (offset) *offset = v;
  return true;
}

// ---- .zip reading (stored and deflated entries; no ZIP64, no encryption) ----
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct ZipEntry {
  std::string name;  // as stored ('/' separated; UTF-8 when flag bit 11, else taken as is)
  uint16_t method = 0, flags = 0;
  uint32_t compressed = 0, size = 0, local_offset = 0;
  bool symlink = false;  // a Unix symbolic link (made on Linux/macOS): its data is a path, never read
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
    e.symlink = (U16(&zip[at + 4]) >> 8) == 3 && ((U32(&zip[at + 38]) >> 16) & 0170000) == 0120000;
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
    // (at most the size the .zip states: a .zip bomb stops there)
    if (!n64dec::Inflate(&zip[data], e.compressed, out, nullptr, e.size) || out.size() != e.size) {
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

bool IsPcPortFontFolder(const fs::path& name) {
  const std::wstring n = Lower(name.filename().wstring());
  return n.rfind(L"font", 0) == 0;
}

bool IsPicture(const fs::path& file) {
  const std::wstring ext = Lower(file.extension().wstring());
  return ext == L".png" || ext == L".jpg" || ext == L".jpeg";
}

PictureSuffix SuffixOf(const fs::path& file) {
  const std::wstring stem = Lower(file.stem().wstring());
  const size_t dash = stem.rfind(L'-');
  if (dash == std::wstring::npos) return PictureSuffix::kNone;
  const std::wstring s = stem.substr(dash + 1);
  return s == L"xbla" ? PictureSuffix::kXbla : s == L"reflect" ? PictureSuffix::kReflect : PictureSuffix::kOther;
}

int HexPackFileId(const fs::path& file) {
  if (!IsPicture(file)) return -1;
  std::wstring stem = file.stem().wstring();
  // <id>-xbla / <id>-reflect: the same id (pd_assets_import.h PictureSuffix).
  if (stem.size() > 4 && stem[4] == L'-' && SuffixOf(file) != PictureSuffix::kOther) stem = stem.substr(0, 4);
  if (stem.size() != 4) return -1;
  int id = 0;
  for (wchar_t c : stem) {
    const int d = HexDigit(c);
    if (d < 0) return -1;
    id = id * 16 + d;
  }
  return id <= kMaxTextureId ? id : -1;
}

bool IsIdPicture(const fs::path& file) { return PackFileId(file) >= 0 || HexPackFileId(file) >= 0; }

fs::path PackFolderOf(const fs::path& picture) {
  fs::path folder = picture.parent_path();
  if (IsXblaFolder(folder.filename()) && !folder.parent_path().empty()) folder = folder.parent_path();
  if (_wcsicmp(folder.filename().c_str(), L"ext_tex") == 0 && !folder.parent_path().empty()) folder = folder.parent_path();
  return folder;
}

PackPictures ScanPack(const fs::path& folder) {
  PackPictures r;
  std::error_code ec;
  std::vector<fs::path> rels;
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec) && IsPicture(it->path())) rels.push_back(it->path().lexically_relative(folder));
  }
  std::sort(rels.begin(), rels.end());
  r.root = FindPackRoot(rels);
  std::vector<fs::path> inside;  // relative to the root
  for (const fs::path& rel : rels)
    if (fs::path in = UnderRoot(rel, r.root); !in.empty()) inside.push_back(in);
  r.pc_port = DecidePcPort(inside, fs::is_directory(folder / r.root / L"xbla", ec));
  auto put = [](std::vector<fs::path>& v, int id, const fs::path& file) {
    if (size_t(id) >= v.size()) v.resize(size_t(id) + 1);
    if (v[size_t(id)].empty()) v[size_t(id)] = file;  // the first by name wins
  };
  for (const fs::path& rel : rels) {
    const fs::path file = folder / rel;
    const PictureSuffix suffix = SuffixOf(rel);
    if (r.pc_port) {
      const fs::path in = UnderRoot(rel, r.root);
      const Place place = in.empty() ? Place::kOther : PlaceOf(in);
      const int id = HexPackFileId(rel);
      if (id >= 0 && place != Place::kOther) {
        if (suffix == PictureSuffix::kXbla) put(r.remastered, id, file);
        else if (suffix == PictureSuffix::kReflect) put(r.reflect, id, file);
        else put(place == Place::kTop ? r.top : r.xbla, id, file);
        continue;
      }
      ModelPicture m;
      if (!in.empty() && ParseModelPicture(in, &m.file, &m.offset)) {
        m.path = file;
        r.models.push_back(std::move(m));
        continue;
      }
    } else if (const int id = PackFileId(rel); id >= 0) {
      // decimal packs: any folder, as before
      put(suffix == PictureSuffix::kXbla ? r.remastered : suffix == PictureSuffix::kReflect ? r.reflect : r.top, id, file);
      continue;
    }
    HiresName n;
    if (ParseHiresName(rel, &n)) {
      if (n.kind != "a") r.hires.push_back(file);
    } else if (IsIdPicture(rel)) {
      ++r.ignored;
    }
  }
  for (const auto* v : {&r.top, &r.xbla, &r.remastered})
    for (const fs::path& p : *v) r.pictures += p.empty() ? 0 : 1;
  r.pictures += int(r.hires.size() + r.models.size());
  return r;
}

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

bool ParseHiresName(const fs::path& file, HiresName* out) {
  if (!IsPicture(file)) return false;
  const std::string stem = Utf8(file.stem().wstring());
  // From the end: "_<kind>", then "#..." fields: crc, fmt, siz[, palette].
  const size_t underscore = stem.rfind('_');
  if (underscore == std::string::npos) return false;
  const std::string kind = stem.substr(underscore + 1);
  std::vector<std::string> fields;
  size_t pos = stem.find('#');
  if (pos == std::string::npos) return false;
  const std::string body = stem.substr(pos + 1, underscore - pos - 1);
  for (size_t start = 0;;) {
    const size_t hash = body.find('#', start);
    fields.push_back(body.substr(start, hash == std::string::npos ? std::string::npos : hash - start));
    if (hash == std::string::npos) break;
    start = hash + 1;
  }
  if (fields.size() != 3 && fields.size() != 4) return false;
  auto hex32 = [](const std::string& s, uint32_t* v) {
    if (s.size() != 8) return false;
    uint32_t r = 0;
    for (char c : s) {
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      r = (r << 4) | uint32_t(d);
    }
    *v = r;
    return true;
  };
  auto one_digit = [](const std::string& s, int* v) {
    if (s.size() != 1 || s[0] < '0' || s[0] > '9') return false;
    *v = s[0] - '0';
    return true;
  };
  HiresName n;
  if (!hex32(fields[0], &n.crc) || !one_digit(fields[1], &n.fmt) || !one_digit(fields[2], &n.siz)) return false;
  if (fields.size() == 4) {
    if (!hex32(fields[3], &n.palette)) return false;
    n.has_palette = true;
  }
  n.kind = kind;
  if (out) *out = n;
  return true;
}

bool IsPackFile(const fs::path& file) { return PackFileId(file) >= 0 || ParseHiresName(file, nullptr); }

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
  // What the header says (game code and revision) names it in the messages; the version
  // itself is decided by the whole ROM's MD5 (.z64 order), so a modified or bad dump isn't
  // taken for a known one.
  const std::string described = DescribeRom(rom);
  const std::string md5 = Md5Hex(rom.data(), rom.size());
  const n64dec::RomVersion* v = n64dec::FindRomVersion(md5);
  if (!v) {
    const bool pd = rom.size() >= 0x40 && rom[0x3B] == 'N' && rom[0x3C] == 'P' && rom[0x3D] == 'D';
    *message = Utf8(src.filename().wstring()) + " is " + described +
               (pd ? ", but not a known clean dump (a modified, patched or bad dump?). The game takes unmodified "
                     "NTSC-U (1.0 and 1.1), PAL and NTSC-J Perfect Dark ROMs."
                   : ": not Perfect Dark.");
    return false;
  }
  if (!v->texture_seg) {
    *message = described + " isn't supported (no texture table is known for it).";
    return false;
  }
  fs::create_directories(dest_folder, ec);
  // The ROM there now (to say what it replaces: HANDOVER 4dv). The same one again is kept.
  std::string before;
  {
    std::vector<uint8_t> old;
    if (ReadFile(dest_folder / "pd.z64", old, 0x2000000) && old.size() == rom.size()) {
      if (old == rom) {
        if (version) *version = v->name;
        *message = "That N64 ROM is already in use (" + described + ", " + v->name + ").";
        return true;
      }
      if (n64dec::NormaliseRomByteOrder(old))
        if (const n64dec::RomVersion* ov = n64dec::FindRomVersion(Md5Hex(old.data(), old.size()))) before = ov->name;
    }
  }
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
  *message = "N64 ROM added (" + described + ", " + v->name + (v->tested ? "" : "; not tested with this version yet") +
             ")" + (!before.empty() && before != v->name ? "; it replaces the " + before + " ROM." : ".");
  return true;
}

std::string SafePackName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')') out += char(c);
    else if (c >= 0x80) continue;  // keep pack names plain ASCII: the game's font has no others
    else out += '_';
    if (out.size() >= 48) break;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  return out;
}

// A release file's name made readable, as the old downloader named packs (HANDOVER 4dl, 4dv):
// dots and underscores become spaces except between digits, so "PD.Forever.Plus.HD.v0.10"
// is "PD Forever Plus HD v0.10".
std::string ReadablePackName(const std::string& stem) {
  std::string out;
  for (size_t i = 0; i < stem.size(); ++i) {
    const char c = stem[i];
    const bool between_digits = i > 0 && i + 1 < stem.size() && std::isdigit(static_cast<unsigned char>(stem[i - 1])) &&
                                std::isdigit(static_cast<unsigned char>(stem[i + 1]));
    out += (c == '.' || c == '_') && !between_digits ? ' ' : c;
  }
  std::string squeezed;
  for (const char c : out)
    if (!(c == ' ' && !squeezed.empty() && squeezed.back() == ' ')) squeezed += c;
  return SafePackName(squeezed);
}


namespace {

// One file of what is being imported, by its path inside the pack (folder or .zip).
struct Item {
  fs::path rel;
  fs::path file;                   // a folder's file
  const ZipEntry* entry = nullptr;  // or a .zip's
};

// A .zip entry's name that could write outside the pack, or isn't a plain relative path:
// absolute ("/x", "\x", "C:..."), with a ".." part, a drive or stream colon, or control
// characters. Such a .zip is refused as a whole (spec: path traversal).
bool UnsafeZipName(const std::string& name) {
  if (name.empty()) return true;
  if (name[0] == '/' || name[0] == '\\') return true;
  for (const unsigned char c : name)
    if (c < 32 || c == ':') return true;
  size_t start = 0;
  for (;;) {
    const size_t sep = name.find_first_of("/\\", start);
    const std::string part = name.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
    if (part == "..") return true;
    if (sep == std::string::npos) break;
    start = sep + 1;
  }
  return false;
}

constexpr uint64_t kMaxNoteBytes = 1ull << 20;  // a .txt / .md beside the pictures
constexpr size_t kMaxEntries = 100000;          // files in a .zip or folder

// The pack being made: written into a staging folder inside the packs folder, then renamed
// into place (so a failed or interrupted import leaves nothing half-made behind).
struct Staging {
  fs::path packs_folder, dir;
  std::string pack;  // the name asked for
  int count = 0, ignored = 0, not_pictures = 0;
  uint64_t total = 0;
  std::string error;

  bool Begin(const fs::path& folder, const std::string& name) {
    std::error_code ec;
    packs_folder = folder;
    pack = name;
    fs::create_directories(packs_folder, ec);
    dir = packs_folder / Wide(".importing-" + pack);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) error = "can't write to " + Utf8(packs_folder.wstring());
    return !ec;
  }
  // Two files for one name: the first by path wins.
  bool Keep(const fs::path& out_rel, bool counts, const uint8_t* data, size_t size) {
    if (count >= kMaxPictures || (total += size) > kMaxPackBytes) {
      error = "the pack is too large (over 20000 pictures or 4 GB)";
      return false;
    }
    std::error_code ec;
    const fs::path out = dir / out_rel;
    // (out_rel is built from a file name and at most one folder name checked above, so it
    // can't leave the staging folder; checked again here all the same)
    const fs::path inside = out.lexically_normal().lexically_relative(dir.lexically_normal());
    if (inside.empty() || *inside.begin() == L"..") {
      error = "a file in it has an unsafe path";
      return false;
    }
    if (fs::exists(out, ec)) return true;
    fs::create_directories(out.parent_path(), ec);
    if (!WriteFile(out, data, size)) {
      error = "can't write to " + Utf8(packs_folder.wstring());
      return false;
    }
    if (counts) ++count;
    return true;
  }
  void Abandon() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// The files of `folder` (relative path -> size), notes and pictures alike.
std::map<fs::path, uint64_t> FilesOf(const fs::path& folder) {
  std::map<fs::path, uint64_t> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) out[it->path().lexically_relative(folder)] = it->file_size(ec);
    if (out.size() > kMaxEntries) break;
  }
  return out;
}

// A pack's pictures (relative path -> size): what decides "the same pack" whatever it is
// called. Notes (.txt, .md, pd-look.txt, a player's own pd-pack.txt) don't count.
std::map<fs::path, uint64_t> PicturesOf(const fs::path& folder) {
  std::map<fs::path, uint64_t> out;
  for (auto& [rel, size] : FilesOf(folder)) {
    const std::wstring ext = rel.extension().wstring();
    if (_wcsicmp(ext.c_str(), L".png") == 0 || _wcsicmp(ext.c_str(), L".jpg") == 0 ||
        _wcsicmp(ext.c_str(), L".jpeg") == 0)
      out[rel] = size;
  }
  return out;
}

// Puts the staged pack in place. An installed pack of the same name is never replaced (the
// spec: no accidental overwrites): the same files again is "already installed"; different
// ones get a free numbered name, "Name (2)". A pack with the same pictures under another
// name (the downloader's "PD Forever Plus HD v0.10" and the release .zip imported by hand,
// HANDOVER 4dv) is "already installed" too. `name` = the name it has now.
bool Finish(Staging& s, std::string* name, bool* already) {
  std::error_code ec;
  *already = false;
  {
    const std::map<fs::path, uint64_t> staged = PicturesOf(s.dir);
    if (!staged.empty()) {
      for (auto it = fs::directory_iterator(s.packs_folder, ec); !ec && it != fs::directory_iterator();
           it.increment(ec)) {
        if (!it->is_directory(ec) || it->path() == s.dir) continue;
        const std::string other = Utf8(it->path().filename().wstring());
        if (other.rfind(".importing-", 0) == 0) continue;
        if (PicturesOf(it->path()) == staged) {
          s.Abandon();
          *name = other;
          *already = true;
          return true;
        }
      }
      ec.clear();
    }
  }
  std::string final_name = s.pack;
  fs::path dest = s.packs_folder / Wide(final_name);
  if (fs::exists(dest, ec)) {
    std::map<fs::path, uint64_t> staged = FilesOf(s.dir), installed = FilesOf(dest);
    installed.erase(fs::path(L"pd-look.txt"));  // (written by the game: not part of the pack)
    if (staged == installed) {
      s.Abandon();
      *name = final_name;
      *already = true;
      return true;
    }
    for (int n = 2;; ++n) {
      if (n > 99) {
        s.Abandon();
        s.error = "there are too many packs named \"" + s.pack + "\"";
        return false;
      }
      std::string base = s.pack;
      const std::string suffix = " (" + std::to_string(n) + ")";
      if (base.size() + suffix.size() > 48) base.resize(48 - suffix.size());
      final_name = base + suffix;
      dest = s.packs_folder / Wide(final_name);
      if (!fs::exists(dest, ec)) break;
      std::map<fs::path, uint64_t> other = FilesOf(dest);
      other.erase(fs::path(L"pd-look.txt"));
      if (other == FilesOf(s.dir)) {
        s.Abandon();
        *name = final_name;
        *already = true;
        return true;
      }
    }
  }
  fs::rename(s.dir, dest, ec);
  if (ec) {
    s.Abandon();
    s.error = "can't write to " + Utf8(s.packs_folder.wstring());
    return false;
  }
  *name = final_name;
  return true;
}

std::string Counted(int count, int ignored, int not_pictures) {
  std::string s = std::to_string(count) + (count == 1 ? " picture" : " pictures");
  if (ignored) s += "; " + std::to_string(ignored) + " in other folders not used";
  if (not_pictures)
    s += "; " + std::to_string(not_pictures) + (not_pictures == 1 ? " file named like a picture but not a real PNG/JPEG"
                                                                  : " files named like pictures but not real PNG/JPEG") +
         " left out";
  return s;
}

// Reads one item's bytes (a .zip entry inflated, at most `max` bytes, or a folder's file).
// False with `fatal` set when the .zip itself is damaged; false alone: skip this file.
bool ReadItem(const std::vector<uint8_t>& zip, const Item& item, uint64_t max, std::vector<uint8_t>& bytes,
              bool* fatal, std::string* error) {
  *fatal = false;
  if (item.entry) {
    if (item.entry->size > max) return false;
    if (!ZipRead(zip, *item.entry, bytes, error)) {
      *fatal = true;
      return false;
    }
    return true;
  }
  return ReadFile(item.file, bytes, max);
}

}  // namespace

bool LooksLikePicture(const uint8_t* data, size_t size) {
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (size >= 8 && std::memcmp(data, kPng, 8) == 0) return true;
  return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;  // JPEG
}

InputKind Identify(const fs::path& path, std::string* detail) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) return InputKind::kFolder;
  if (!fs::is_regular_file(path, ec)) {
    if (detail) *detail = "it can't be read";
    return InputKind::kUnknown;
  }
  uint8_t head[64] = {};
  std::ifstream f(path, std::ios::binary);
  f.read(reinterpret_cast<char*>(head), sizeof(head));
  const size_t got = size_t(f.gcount());
  const uint64_t size = fs::file_size(path, ec);
  if (got >= 4 && head[0] == 'P' && head[1] == 'K' &&
      ((head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6) || (head[2] == 7 && head[3] == 8)))
    return InputKind::kZip;
  if (got >= 4) {
    const uint8_t* m = head;
    const bool rom = (m[0] == 0x80 && m[1] == 0x37 && m[2] == 0x12 && m[3] == 0x40) ||
                     (m[0] == 0x37 && m[1] == 0x80 && m[2] == 0x40 && m[3] == 0x12) ||
                     (m[0] == 0x40 && m[1] == 0x12 && m[2] == 0x37 && m[3] == 0x80);
    if (rom) return InputKind::kRom;
  }
  if (LooksLikePicture(head, got)) return InputKind::kPicture;
  if (detail) {
    const std::wstring ext = Lower(path.extension().wstring());
    if (ext == L".z64" || ext == L".n64" || ext == L".v64")
      *detail = "it isn't an N64 ROM (its start isn't an N64 ROM header)";
    else if (ext == L".zip")
      *detail = "it isn't a .zip file";
    else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg")
      *detail = "it isn't a real PNG or JPEG picture";
    else if (ext == L".7z" || ext == L".rar")
      *detail = "7-Zip and RAR archives aren't read: extract it and import the folder";
    else
      *detail = size == 0 ? "it is empty" : "it isn't an N64 ROM, a .zip or a PNG/JPEG picture";
  }
  return InputKind::kUnknown;
}

std::string DescribeRom(const std::vector<uint8_t>& rom) {
  if (rom.size() < 0x40) return "an N64 ROM";
  std::string title;
  for (size_t i = 0x20; i < 0x34; ++i) {
    const char c = char(rom[i]);
    title += (c >= 32 && c < 127) ? c : ' ';
  }
  while (!title.empty() && title.back() == ' ') title.pop_back();
  char code[5] = {};
  for (int i = 0; i < 4; ++i) {
    const char c = char(rom[size_t(0x3B + i)]);
    code[i] = (c >= 32 && c < 127) ? c : '?';
  }
  const int revision = rom[0x3F];
  if (std::string(code, 3) != "NPD") return "\"" + (title.empty() ? std::string("an N64 ROM") : title) + "\" (" + code + ")";
  const char* region = code[3] == 'E' ? "NTSC-U (North America)"
                       : code[3] == 'P' ? "PAL (Europe/Australia)"
                       : code[3] == 'J' ? "NTSC-J (Japan)"
                                        : "of an unknown region";
  return std::string("Perfect Dark ") + region + ", revision " + std::to_string(revision) + " (" + code + ")";
}

bool ImportTexturePack(const fs::path& src, const fs::path& packs_folder, std::string* name, int* pictures,
                       std::string* message) {
  std::error_code ec;
  const bool is_folder = fs::is_directory(src, ec);
  const bool is_zip = !is_folder && Identify(src, nullptr) == InputKind::kZip;
  if (!is_folder && !is_zip) {
    *message = "Texture pack not added: " + Utf8(src.filename().wstring()) + " isn't a folder or a .zip file.";
    return false;
  }
  const std::string pack = ReadablePackName(Utf8((is_zip ? src.stem() : src.filename()).wstring()));
  if (pack.empty()) { *message = "Texture pack not added: it needs a name made of letters or digits."; return false; }
  if (is_folder && fs::equivalent(src, packs_folder / Wide(pack), ec)) {
    // A pack already in the packs folder: nothing to copy.
    const PackPictures installed = ScanPack(src);
    if (name) *name = pack;
    if (pictures) *pictures = installed.pictures;
    *message = "Texture pack \"" + pack + "\" is already installed (" + Counted(installed.pictures, installed.ignored, 0) + ").";
    return true;
  }
  const fs::path packs_canon = fs::weakly_canonical(packs_folder, ec);
  if (is_folder) {
    // The packs folder itself, or a folder holding it, would be copied into itself.
    const fs::path inside = packs_canon.lexically_relative(fs::weakly_canonical(src, ec));
    if (!inside.empty() && *inside.begin() != L"..") {
      *message = "Texture pack not added: choose a pack's own folder, not one that holds the texture-packs folder.";
      return false;
    }
  }

  // The given files by their path inside the folder / .zip.
  std::vector<Item> items;
  std::vector<ZipEntry> entries;
  std::vector<size_t> central;  // (entries' central-directory places, for the symlink check)
  std::vector<uint8_t> zip;
  std::vector<fs::path> folders;  // (for an empty xbla folder)
  std::string error;
  bool ok = true;
  int links = 0;
  if (is_folder) {
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->is_symlink(ec)) {  // a link (to a file or folder elsewhere) is never followed
        ++links;
        if (it->is_directory(ec)) it.disable_recursion_pending();
        continue;
      }
      if (it->is_directory(ec)) folders.push_back(it->path().lexically_relative(src));
      else if (it->is_regular_file(ec)) items.push_back({it->path().lexically_relative(src), it->path(), nullptr});
      if (items.size() + folders.size() > kMaxEntries) {
        error = "it has too many files (over 100000)";
        ok = false;
        break;
      }
    }
  } else {
    if (!ReadFile(src, zip, kMaxPackBytes)) {
      error = "can't read the .zip (or it is over 4 GB)";
      ok = false;
    }
    if (ok && !ZipEntries(zip, entries, &error)) ok = false;
    if (ok && entries.size() > kMaxEntries) {
      error = "it has too many files";
      ok = false;
    }
    if (ok) {
      uint64_t declared = 0;
      for (const ZipEntry& e : entries) {
        if (UnsafeZipName(e.name)) {
          error = "a file in it has an unsafe path (\"" + e.name.substr(0, 60) + "\"): the .zip was refused";
          ok = false;
          break;
        }
        declared += e.size;
      }
      if (ok && declared > kMaxPackBytes) {
        error = "it would unpack to over 4 GB";
        ok = false;
      }
    }
    if (ok) {
      for (const ZipEntry& e : entries) {
        if (e.symlink) {
          ++links;
          continue;
        }
        const fs::path rel = fs::path(Wide(e.name)).lexically_normal();
        if (e.name.back() == '/' || e.name.back() == '\\') folders.push_back(rel.parent_path());
        else items.push_back({rel, {}, &e});
      }
    }
  }
  if (!ok) {
    *message = "Texture pack not added: " + error + ".";
    return false;
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rel < b.rel; });
  std::vector<fs::path> rels;
  for (const Item& i : items)
    if (IsPicture(i.rel)) rels.push_back(i.rel);
  const fs::path root = FindPackRoot(rels);
  bool xbla_folder = false;
  std::vector<fs::path> inside;
  for (const fs::path& rel : rels)
    if (fs::path in = UnderRoot(rel, root); !in.empty()) {
      inside.push_back(in);
      if (PlaceOf(in) == Place::kXbla) xbla_folder = true;
    }
  for (const fs::path& f : folders) {
    const fs::path in = UnderRoot(f, root);
    if (!in.empty() && !in.has_parent_path() && IsXblaFolder(in)) xbla_folder = true;
  }
  const bool pc_port = DecidePcPort(inside, xbla_folder);
  auto at_or_above_root = [&root](const fs::path& dir) {
    if (dir.empty()) return true;
    const fs::path r = root.lexically_relative(dir);  // "." when dir is the root
    return !r.empty() && *r.begin() != L"..";
  };

  Staging s;
  if (!s.Begin(packs_folder, pack)) {
    *message = "Texture pack not added: " + s.error + ".";
    return false;
  }
  if (pc_port && xbla_folder) fs::create_directories(s.dir / L"xbla", ec);  // (marks the pack, even if empty)
  for (const Item& item : items) {
    if (!ok) break;
    const fs::path in = UnderRoot(item.rel, root);
    const Place place = in.empty() ? Place::kOther : PlaceOf(in);
    const fs::path file_name = item.rel.filename();
    fs::path out_rel;
    bool counts = true, note = false;
    HiresName n;
    if (pc_port && HexPackFileId(file_name) >= 0 && place != Place::kOther) {
      out_rel = place == Place::kXbla ? fs::path(L"xbla") / file_name : file_name;
      counts = SuffixOf(file_name) != PictureSuffix::kReflect;  // (a mask is a setting, not a picture)
    } else if (pc_port && !in.empty() && ParseModelPicture(in, nullptr, nullptr)) {
      out_rel = in;  // <model file>\<offset>: kept as it is
    } else if (!pc_port && PackFileId(file_name) >= 0) {
      out_rel = file_name;  // decimal packs: stored flat, from any folder, as before
      counts = SuffixOf(file_name) != PictureSuffix::kReflect;
    } else if (ParseHiresName(file_name, &n)) {
      out_rel = file_name;
      counts = n.kind != "a";
    } else if (IsNote(file_name) && at_or_above_root(item.rel.parent_path())) {
      out_rel = file_name;  // notes in the pack's folder or the folders around it (pd-pack.txt)
      counts = false;
      note = true;
    } else {
      if (IsIdPicture(file_name)) ++s.ignored;
      continue;  // anything else (programs, scripts, other files) is never copied
    }
    std::vector<uint8_t> bytes;
    bool fatal = false;
    if (!ReadItem(zip, item, note ? kMaxNoteBytes : kMaxPictureBytes, bytes, &fatal, &error)) {
      if (fatal) ok = false;
      continue;  // too large or unreadable: left out
    }
    if (!note && !LooksLikePicture(bytes.data(), bytes.size())) {
      ++s.not_pictures;  // named like a picture, but not one: left out
      continue;
    }
    if (!s.Keep(out_rel, counts, bytes.data(), bytes.size())) {
      error = s.error;
      ok = false;
    }
  }
  if (ok && s.count == 0) {
    error = s.not_pictures ? "none of its pictures are real PNG or JPEG files"
                           : "it has no pictures named by texture number (like 0123.png or 00C6.jpg) or by N64 "
                             "emulator checksum";
    ok = false;
  }
  if (!ok) {
    s.Abandon();
    *message = "Texture pack not added: " + error + ".";
    return false;
  }
  std::string final_name;
  bool already = false;
  if (!Finish(s, &final_name, &already)) {
    *message = "Texture pack not added: " + s.error + ".";
    return false;
  }
  if (name) *name = final_name;
  if (pictures) *pictures = s.count;
  std::string extra;
  if (links) extra = "; " + std::to_string(links) + " links not followed";
  *message = already ? "Texture pack \"" + final_name + "\" is already installed (" +
                           (final_name == pack ? std::string("the same files") : "the same pictures as " + Utf8(src.filename().wstring())) + ")."
                     : "Texture pack \"" + final_name + "\" added (" + Counted(s.count, s.ignored, s.not_pictures) +
                           extra + ")." +
                           (final_name != pack ? " (\"" + pack + "\" was taken by another pack.)" : "");
  return true;
}

bool ImportPictures(const std::vector<fs::path>& files, const fs::path& packs_folder, std::string* name, int* pictures,
                    std::string* message) {
  // Which naming: a PC-port pack if any picture has a four-hex-digit name with a letter, or
  // comes from an xbla folder; decimal otherwise (as the pack rule, pd_assets_import.h).
  std::vector<fs::path> rels;
  for (const fs::path& f : files) {
    const bool in_xbla = IsXblaFolder(f.parent_path().filename());
    rels.push_back(in_xbla ? fs::path(L"xbla") / f.filename() : f.filename());
  }
  const bool pc_port = DecidePcPort(rels, false);
  // The pack's name: the pictures' folder (the one above an xbla folder).
  fs::path folder = files.empty() ? fs::path() : PackFolderOf(files.front());
  std::string pack = ReadablePackName(Utf8(folder.filename().wstring()));
  if (pack.empty() || _wcsicmp(folder.filename().c_str(), L"texture-packs") == 0) pack = "Imported pictures";
  Staging s;
  if (!s.Begin(packs_folder, pack)) {
    *message = "Pictures not added: " + s.error + ".";
    return false;
  }
  int unnamed = 0;
  std::string first_unnamed;
  for (size_t i = 0; i < files.size(); ++i) {
    const fs::path& file = files[i];
    const fs::path& rel = rels[i];
    fs::path out_rel;
    bool counts = true;
    HiresName n;
    if (pc_port && HexPackFileId(file) >= 0) {
      out_rel = rel;  // (top folder, or xbla\ when picked from an xbla folder)
    } else if (!pc_port && PackFileId(file) >= 0) {
      out_rel = file.filename();
    } else if (ParseHiresName(file, &n)) {
      out_rel = file.filename();
      counts = n.kind != "a";
    } else {
      if (!unnamed++) first_unnamed = Utf8(file.filename().wstring());
      continue;
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(file, bytes, kMaxPictureBytes)) continue;
    if (!LooksLikePicture(bytes.data(), bytes.size())) {
      ++s.not_pictures;
      continue;
    }
    if (!s.Keep(out_rel, counts, bytes.data(), bytes.size())) {
      s.Abandon();
      *message = "Pictures not added: " + s.error + ".";
      return false;
    }
  }
  if (pc_port) {
    std::error_code ec;
    fs::create_directories(s.dir / L"xbla", ec);
  }
  if (s.count == 0) {
    s.Abandon();
    *message = unnamed ? "Pictures not added: " + first_unnamed +
                             (unnamed > 1 ? " and the others aren't" : " isn't") +
                             " named by a texture number (like 0123.png, 00C6.jpg) or an N64 emulator checksum, so "
                             "the game can't tell which texture " +
                             (unnamed > 1 ? "they replace." : "it replaces.")
                       : "Pictures not added: none of them are real PNG or JPEG files.";
    return false;
  }
  std::string final_name;
  bool already = false;
  if (!Finish(s, &final_name, &already)) {
    *message = "Pictures not added: " + s.error + ".";
    return false;
  }
  if (name) *name = final_name;
  if (pictures) *pictures = s.count;
  if (already) {
    *message = "Texture pack \"" + final_name + "\" is already installed (the same pictures).";
  } else {
    *message = "Texture pack \"" + final_name + "\" added from the pictures (" +
               Counted(s.count, 0, s.not_pictures) + ")." +
               (unnamed ? " " + std::to_string(unnamed) + " not named by a texture number left out." : "");
  }
  return true;
}

}  // namespace pd::assets
