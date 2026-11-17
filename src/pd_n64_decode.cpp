// N64 Perfect Dark texture decoder: see pd_n64_decode.h and N64-NOTES.md
// (section 1). Ported from tools/n64/n64_tex.py, our own analysis script; the
// N64 decompilation was read as documentation only.
#include "pd_n64_decode.h"

#include <algorithm>
#include <cstring>

namespace pd::n64dec {

namespace {

// --- ROM versions -------------------------------------------------------------

const std::vector<RomVersion> kVersions = {
    // name          md5 of the .z64                     textures   data length  tested
    {"pal-final", "d9b5cd305d228424891ce38e71bc9213", 0x1D5CA20, 0x291D60, true, 0x39850, 0x28910},
    {"ntsc-final", "e03b088b6ac9e0080440efed07c1e40f", 0x1D65F40, 0x291D60, false, 0x39850, 0x28080},
    {"ntsc-1.0", "7f4171b0c8d17815be37913f535e4e93", 0x1D65F40, 0x291D60, false, 0x39850, 0x28080},
    // The Japanese release has 3511 textures (8 more): its ids may not line up
    // with the XBLA's, so it is named but not used.
    {"jpn-final", "538d2b75945eae069b29c46193e74790", 0, 0, false},
};

// --- raw deflate (RFC 1951) -----------------------------------------------------

struct InBits {
  const uint8_t* p;
  size_t n, pos = 0;  // byte position
  uint32_t buf = 0, cnt = 0;
  bool bad = false;
  uint32_t bits(uint32_t need) {
    while (cnt < need) {
      if (pos >= n) { bad = true; return 0; }
      buf |= uint32_t(p[pos++]) << cnt;
      cnt += 8;
    }
    const uint32_t v = buf & ((1u << need) - 1);
    buf >>= need;
    cnt -= need;
    return v;
  }
  void align() { buf = 0; cnt = 0; }
};

// Canonical Huffman code: count of codes per length, symbols in code order.
struct Huff {
  uint16_t count[16] = {};
  uint16_t symbol[320] = {};
};

bool build_huff(Huff& h, const uint8_t* lengths, int n) {
  std::memset(h.count, 0, sizeof(h.count));
  for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
  if (h.count[0] == n) return true;  // no codes (only valid for an unused distance code)
  int left = 1;
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return false;  // over-subscribed
  }
  uint16_t offs[16] = {};
  for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
  for (int i = 0; i < n; ++i)
    if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
  return true;
}

int decode_sym(InBits& in, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    code |= int(in.bits(1));
    if (in.bad) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(InBits& in, std::vector<uint8_t>& out, size_t start, const Huff& lit, const Huff& dist) {
  for (;;) {
    const int sym = decode_sym(in, lit);
    if (sym < 0) return false;
    if (sym < 256) {
      out.push_back(uint8_t(sym));
    } else if (sym == 256) {
      return true;
    } else {
      const int li = sym - 257;
      if (li >= 29) return false;
      const uint32_t len = kLenBase[li] + in.bits(kLenExtra[li]);
      const int di = decode_sym(in, dist);
      if (di < 0 || di >= 30) return false;
      const uint32_t d = kDistBase[di] + in.bits(kDistExtra[di]);
      if (in.bad || d > out.size() - start) return false;
      const size_t from = out.size() - d;
      for (uint32_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
  }
}

// --- Rare's bit-packed texture coder (MSB first) -----------------------------

struct Bits {
  const uint8_t* d;
  size_t size;
  size_t pos = 0;  // in bits
  uint32_t byte_at(size_t i) const { return i < size ? d[i] : 0; }
  uint32_t read(int n) {
    if (n == 0) return 0;
    uint64_t v = 0;
    const size_t b = pos >> 3;
    for (int k = 0; k < 6; ++k) v = (v << 8) | byte_at(b + k);
    const int o = int(pos & 7);
    pos += size_t(n);
    return uint32_t((v >> (48 - o - n)) & ((1ull << n) - 1));
  }
  uint32_t bit() {
    const size_t p = pos++;
    return (byte_at(p >> 3) >> (7 - (p & 7))) & 1;
  }
  bool past_end() const { return (pos >> 3) > size + 8; }
};

// Format tables, indexed by the 4-bit format.
//   0 RGBA32  1 RGBA16  2 RGB24  3 RGB15  4 IA16  5 IA8  6 IA4  7 I8  8 I4
//   (9-12, the CI formats, only appear in zlib textures.)
const int kChannels[9] = {4, 3, 3, 3, 2, 2, 1, 1, 1};
const int kAlpha1[9] = {0, 1, 0, 0, 0, 0, 1, 0, 0};
const int kChanSize[9] = {256, 32, 256, 32, 256, 16, 8, 256, 16};
const int kBpp[9] = {32, 16, 24, 15, 16, 8, 4, 8, 4};

bool huffman(Bits& bs, size_t count, int chansize, std::vector<int>& out) {
  if (chansize <= 0 || chansize > 4096) return false;
  constexpr int kLeaf = 10000, kNone = 9999;
  std::vector<int> freq(static_cast<size_t>(chansize));
  for (int i = 0; i < chansize; ++i) freq[size_t(i)] = int(bs.read(8));
  std::vector<int> n0(size_t(chansize), -1), n1(size_t(chansize), -1);
  int m1, m2, i1, i2;
  const auto two_smallest = [&]() {
    m1 = m2 = kNone;
    i1 = i2 = 0;
    for (int i = 0; i < chansize; ++i) {
      const int f = freq[size_t(i)];
      if (f < m1) {
        if (m1 > m2) { m1 = f; i1 = i; }
        else { m2 = f; i2 = i; }
      } else if (f < m2) {
        m2 = f;
        i2 = i;
      }
    }
  };
  two_smallest();
  int root = 0;
  for (;;) {
    int s = (freq[size_t(i1)] + freq[size_t(i2)]) & 0xFFFF;
    if (s == 0) s = 1;
    freq[size_t(i1)] = kNone;
    freq[size_t(i2)] = kNone;
    const bool leaf1 = n0[size_t(i1)] < 0 && n1[size_t(i1)] < 0;
    const bool leaf2 = n0[size_t(i2)] < 0 && n1[size_t(i2)] < 0;
    if (leaf1) {
      n0[size_t(i1)] = i1 + kLeaf;
      root = i1;
      freq[size_t(i1)] = s;
      n1[size_t(i1)] = leaf2 ? i2 + kLeaf : i2;
    } else if (leaf2) {
      n0[size_t(i2)] = i2 + kLeaf;
      root = i2;
      freq[size_t(i2)] = s;
      n1[size_t(i2)] = leaf1 ? i1 + kLeaf : i1;
    } else {
      int r = 0;
      while (r < chansize && (n0[size_t(r)] >= 0 || n1[size_t(r)] >= 0 || freq[size_t(r)] < kNone)) ++r;
      if (r >= chansize) return false;
      root = r;
      freq[size_t(r)] = s;
      n0[size_t(r)] = i1;
      n1[size_t(r)] = i2;
    }
    two_smallest();
    if (m1 == kNone || m2 == kNone) break;
  }
  out.reserve(out.size() + count);
  for (size_t k = 0; k < count; ++k) {
    int v = root;
    int guard = 0;
    while (v < kLeaf) {
      if (v < 0 || v >= chansize || ++guard > 64) return false;
      v = bs.bit() ? n1[size_t(v)] : n0[size_t(v)];
    }
    out.push_back(v - kLeaf);
  }
  return !bs.past_end();
}

bool rle(Bits& bs, size_t total, std::vector<int>& out) {
  const int btsize = int(bs.read(3)), rlsize = int(bs.read(3)), blocksize = int(bs.read(4));
  int cost = btsize + rlsize + blocksize + 1, fudge = 0;
  while (cost > 0) {
    cost -= blocksize + 1;
    ++fudge;
  }
  const size_t base = out.size();
  while (out.size() - base < total) {
    if (bs.past_end()) return false;
    if (bs.read(1) == 0) {
      out.push_back(int(bs.read(blocksize)));
    } else {
      const size_t back = size_t(bs.read(btsize)) + 1;
      const size_t len = size_t(bs.read(rlsize)) + size_t(fudge);
      if (back > out.size() - base) return false;
      const size_t start = out.size() - back;
      for (size_t i = start; i < start + len; ++i) out.push_back(out[i]);
      out.push_back(int(bs.read(blocksize)));
    }
  }
  out.resize(base + total);
  return true;
}

void blur(std::vector<int>& px, int width, int height, int method, int chansize) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t i = size_t(y) * size_t(width) + size_t(x);
      const int cur = px[i] + chansize * 2;
      const int left = x > 0 ? px[i - 1] : 0;
      const int above = y > 0 ? px[i - size_t(width)] : 0;
      const int al = x > 0 && y > 0 ? px[i - size_t(width) - 1] : 0;
      int v;
      switch (method) {
        case 0: v = cur + left; break;
        case 1: v = cur + above; break;
        case 2: v = cur + al; break;
        case 3: v = cur + (left + above - al); break;
        case 4: v = cur + ((above - al) / 2 + left); break;  // C++ division truncates, as the format's does
        case 5: v = cur + ((left - al) / 2 + above); break;
        case 6: v = cur + (left + above) / 2; break;
        default: continue;
      }
      int m = v % chansize;
      if (m < 0) m += chansize;
      px[i] = m & 0xFF;
    }
  }
}

int bitsize(int n) {
  int c = 0;
  --n;
  while (n > 0) {
    n >>= 1;
    ++c;
  }
  return c;
}

uint32_t c5(uint32_t v) { return (v << 3) | (v >> 2); }

void put_rgba(int fmt, uint32_t value, uint8_t* o) {
  switch (fmt) {
    case 0: o[0] = uint8_t(value >> 24); o[1] = uint8_t(value >> 16); o[2] = uint8_t(value >> 8); o[3] = uint8_t(value); return;
    case 2: o[0] = uint8_t(value >> 24); o[1] = uint8_t(value >> 16); o[2] = uint8_t(value >> 8); o[3] = 255; return;
    case 1:
    case 3:
      o[0] = uint8_t(c5((value >> 11) & 31)); o[1] = uint8_t(c5((value >> 6) & 31)); o[2] = uint8_t(c5((value >> 1) & 31));
      o[3] = (value & 1) ? 255 : 0;
      return;
    case 4: o[0] = o[1] = o[2] = uint8_t(value >> 8); o[3] = uint8_t(value); return;
    case 5: o[0] = o[1] = o[2] = uint8_t(((value >> 4) & 15) * 17); o[3] = uint8_t((value & 15) * 17); return;
    case 6: o[0] = o[1] = o[2] = uint8_t(((value >> 1) & 7) * 255 / 7); o[3] = (value & 1) ? 255 : 0; return;
    case 7: o[0] = o[1] = o[2] = uint8_t(value); o[3] = 255; return;
    case 8: o[0] = o[1] = o[2] = uint8_t((value & 15) * 17); o[3] = 255; return;
    default: o[0] = o[1] = o[2] = o[3] = 0; return;
  }
}

bool channels_to_values(const std::vector<int>& ch, size_t n, int fmt, std::vector<uint32_t>& vals) {
  const size_t need = fmt == 0 || fmt == 1 ? 4 * n : fmt == 2 || fmt == 3 ? 3 * n : fmt == 4 || fmt == 5 ? 2 * n
                      : fmt == 6 ? 4 * n : n;
  if (ch.size() < need) return false;
  vals.resize(n);
  const auto c = [&](size_t i) { return uint32_t(ch[i]); };
  for (size_t i = 0; i < n; ++i) {
    switch (fmt) {
      case 0: vals[i] = (c(i) << 24) | (c(i + n) << 16) | (c(i + 2 * n) << 8) | c(i + 3 * n); break;
      case 2: vals[i] = (c(i) << 24) | (c(i + n) << 16) | (c(i + 2 * n) << 8) | 255; break;
      case 1: vals[i] = (c(i) << 11) | (c(i + n) << 6) | (c(i + 2 * n) << 1) | c(i + 3 * n); break;
      case 3: vals[i] = (c(i) << 11) | (c(i + n) << 6) | (c(i + 2 * n) << 1) | 1; break;
      case 4: vals[i] = (c(i) << 8) | c(i + n); break;
      case 5: vals[i] = (c(i) << 4) | c(i + n); break;
      case 6: vals[i] = (c(i) << 1) | c(i + 3 * n); break;
      default: vals[i] = c(i); break;
    }
  }
  return true;
}

void read_raw(Bits& bs, int w, int h, int fmt, std::vector<uint32_t>& vals) {
  const size_t n = size_t(w) * size_t(h);
  vals.clear();
  vals.reserve(n);
  switch (fmt) {
    case 0: for (size_t i = 0; i < n; ++i) { const uint32_t hi = bs.read(16); vals.push_back((hi << 16) | bs.read(16)); } break;
    case 2: for (size_t i = 0; i < n; ++i) vals.push_back((bs.read(24) << 8) | 255); break;
    case 1: case 4: for (size_t i = 0; i < n; ++i) vals.push_back(bs.read(16)); break;
    case 3: for (size_t i = 0; i < n; ++i) vals.push_back((bs.read(15) << 1) | 1); break;
    case 5: case 7: for (size_t i = 0; i < n; ++i) vals.push_back(bs.read(8)); break;
    default:  // 6, 8: 4 bits a texel, rows padded to whole bytes
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; x += 2) {
          const uint32_t b = bs.read(8);
          vals.push_back(b >> 4);
          if (x + 1 < w) vals.push_back(b & 15);
        }
      }
      break;
  }
}

bool build_lookup(Bits& bs, int bpp, std::vector<uint32_t>& table) {
  const uint32_t n = bs.read(11);
  table.clear();
  table.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (bpp <= 24) {
      table.push_back(bs.read(bpp));
    } else {
      const uint32_t hi = bs.read(24);
      table.push_back((hi << 8) | bs.read(bpp - 24));
    }
  }
  return n > 0 && !bs.past_end();
}

uint32_t lookup_value(int fmt, uint32_t t) {
  switch (fmt) {
    case 2: return (t << 8) | 255;
    case 3: return (t << 1) | 1;
    case 5: case 7: return t & 255;
    case 6: case 8: return t & 15;
    default: return t;
  }
}

bool decode_bitpacked(const uint8_t* d, size_t size, Image& img, std::string* error) {
  Bits bs{d, size};
  const int fmt = int(bs.read(4)), w = int(bs.read(8)), h = int(bs.read(8)), method = int(bs.read(4));
  if (fmt > 8 || w == 0 || h == 0) {
    if (error) *error = "bad header (format " + std::to_string(fmt) + ")";
    return false;
  }
  const size_t n = size_t(w) * size_t(h);
  const int nch = kChannels[fmt];
  std::vector<uint32_t> vals;
  if (method == 0 || method == 1) {
    read_raw(bs, w, h, fmt, vals);
  } else if (method == 2 || method == 3 || method == 4 || method == 8 || method == 9) {
    const int blurm = method == 8 || method == 9 ? int(bs.read(3)) : -1;
    std::vector<int> ch;
    bool ok = true;
    if (method == 3) {
      for (int c = 0; c < nch && ok; ++c) ok = huffman(bs, n, kChanSize[fmt], ch);
    } else if (method == 2 || method == 8) {
      ok = huffman(bs, size_t(nch) * n, kChanSize[fmt], ch);
    } else {
      ok = rle(bs, size_t(nch) * n, ch);
    }
    if (!ok) {
      if (error) *error = "bad Huffman/run data (method " + std::to_string(method) + ")";
      return false;
    }
    if (blurm >= 0) blur(ch, w, nch * h, blurm, kChanSize[fmt]);
    if (kAlpha1[fmt]) {
      ch.resize(std::max(ch.size(), 3 * n), 0);
      ch.resize(3 * n);
      for (size_t i = 0; i < n; ++i) ch.push_back(int(bs.read(1)));
    }
    if (!channels_to_values(ch, n, fmt, vals)) {
      if (error) *error = "short channel data";
      return false;
    }
  } else if (method == 5 || method == 6 || method == 7) {
    std::vector<uint32_t> table;
    if (!build_lookup(bs, kBpp[fmt], table)) {
      if (error) *error = "bad lookup table";
      return false;
    }
    vals.resize(n);
    if (method == 5) {
      const int bits = bitsize(int(table.size()));
      for (size_t i = 0; i < n; ++i) {
        const uint32_t k = bs.read(bits);
        if (k >= table.size()) { if (error) *error = "lookup index out of range"; return false; }
        vals[i] = lookup_value(fmt, table[k]);
      }
    } else {
      std::vector<int> idx;
      const bool ok = method == 6 ? huffman(bs, n, int(table.size()), idx) : rle(bs, n, idx);
      if (!ok || idx.size() < n) { if (error) *error = "bad lookup indices"; return false; }
      for (size_t i = 0; i < n; ++i) {
        if (idx[i] < 0 || size_t(idx[i]) >= table.size()) { if (error) *error = "lookup index out of range"; return false; }
        vals[i] = lookup_value(fmt, table[size_t(idx[i])]);
      }
    }
  } else {
    if (error) *error = "unknown method " + std::to_string(method);
    return false;
  }
  if (vals.size() < n || bs.past_end()) {
    if (error) *error = "texture data ends early";
    return false;
  }
  img.w = uint16_t(w);
  img.format = uint8_t(fmt);
  img.h = uint16_t(h);
  img.rgba.resize(n * 4);
  for (size_t i = 0; i < n; ++i) put_rgba(fmt, vals[i], img.rgba.data() + i * 4);
  return true;
}

bool decode_zlib(const uint8_t* d, size_t size, Image& img, std::string* error) {
  if (size < 2) return false;
  const int fmt = d[0];
  const size_t ncol = size_t(d[1]) + 1;
  size_t pos = 2 + 2 * ncol;
  if (fmt < 9 || fmt > 12 || pos + 7 > size) {
    if (error) *error = "bad paletted header (format " + std::to_string(fmt) + ")";
    return false;
  }
  std::vector<uint16_t> pal(ncol);
  for (size_t i = 0; i < ncol; ++i) pal[i] = uint16_t((d[2 + 2 * i] << 8) | d[3 + 2 * i]);
  const int w = d[pos], h = d[pos + 1];
  pos += 2;
  if (d[pos] != 0x11 || d[pos + 1] != 0x73 || w == 0 || h == 0) {
    if (error) *error = "bad rarezip header";
    return false;
  }
  std::vector<uint8_t> raw;
  if (!Inflate(d + pos + 5, size - pos - 5, raw)) {
    if (error) *error = "bad deflate data";
    return false;
  }
  const bool ci4 = fmt == 10 || fmt == 12;
  const size_t stride = ci4 ? (size_t(w) + 1) / 2 : size_t(w);
  if (raw.size() < stride * size_t(h)) {
    if (error) *error = "short index data";
    return false;
  }
  img.w = uint16_t(w);
  img.format = uint8_t(fmt);
  img.h = uint16_t(h);
  img.rgba.resize(size_t(w) * size_t(h) * 4);
  uint8_t* o = img.rgba.data();
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = raw.data() + size_t(y) * stride;
    for (int x = 0; x < w; ++x, o += 4) {
      const size_t i = ci4 ? ((x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4)) : row[x];
      const uint32_t c = i < ncol ? pal[i] : 0;
      put_rgba(fmt == 9 || fmt == 10 ? 1 : 4, c, o);
    }
  }
  return true;
}

}  // namespace

const std::vector<RomVersion>& KnownRomVersions() { return kVersions; }

const RomVersion* FindRomVersion(const std::string& md5_hex) {
  for (const RomVersion& v : kVersions)
    if (md5_hex == v.md5) return &v;
  return nullptr;
}

bool NormaliseRomByteOrder(std::vector<uint8_t>& rom) {
  if (rom.size() < 0x1000 || rom.size() % 4) return false;
  const uint8_t* m = rom.data();
  if (m[0] == 0x80 && m[1] == 0x37 && m[2] == 0x12 && m[3] == 0x40) return true;  // .z64
  if (m[0] == 0x37 && m[1] == 0x80 && m[2] == 0x40 && m[3] == 0x12) {              // .v64
    for (size_t i = 0; i < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    return true;
  }
  if (m[0] == 0x40 && m[1] == 0x12 && m[2] == 0x37 && m[3] == 0x80) {  // .n64
    for (size_t i = 0; i < rom.size(); i += 4) {
      std::swap(rom[i], rom[i + 3]);
      std::swap(rom[i + 1], rom[i + 2]);
    }
    return true;
  }
  return false;
}

bool Inflate(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out, size_t* consumed) {
  InBits bs{in, in_size};
  const size_t start = out.size();
  Huff lit, dist;
  for (;;) {
    const uint32_t final_block = bs.bits(1), type = bs.bits(2);
    if (bs.bad) return false;
    if (type == 0) {  // stored
      bs.align();
      if (bs.pos + 4 > in_size) return false;
      const uint32_t len = in[bs.pos] | (in[bs.pos + 1] << 8), nlen = in[bs.pos + 2] | (in[bs.pos + 3] << 8);
      bs.pos += 4;
      if ((len ^ 0xFFFF) != nlen || bs.pos + len > in_size) return false;
      out.insert(out.end(), in + bs.pos, in + bs.pos + len);
      bs.pos += len;
    } else if (type == 1) {  // fixed codes
      uint8_t lengths[320];
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      build_huff(lit, lengths, 288);
      for (i = 0; i < 30; ++i) lengths[i] = 5;
      build_huff(dist, lengths, 30);
      if (!inflate_codes(bs, out, start, lit, dist)) return false;
    } else if (type == 2) {  // dynamic codes
      const int nlen = int(bs.bits(5)) + 257, ndist = int(bs.bits(5)) + 1, ncode = int(bs.bits(4)) + 4;
      if (bs.bad || nlen > 286 || ndist > 30) return false;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(bs.bits(3));
      Huff lencode;
      if (bs.bad || !build_huff(lencode, lengths, 19)) return false;
      int index = 0;
      uint8_t all[320] = {};
      while (index < nlen + ndist) {
        const int sym = decode_sym(bs, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
          all[index++] = uint8_t(sym);
        } else {
          uint8_t len = 0;
          int rep;
          if (sym == 16) {
            if (index == 0) return false;
            len = all[index - 1];
            rep = 3 + int(bs.bits(2));
          } else if (sym == 17) {
            rep = 3 + int(bs.bits(3));
          } else {
            rep = 11 + int(bs.bits(7));
          }
          if (bs.bad || index + rep > nlen + ndist) return false;
          while (rep--) all[index++] = len;
        }
      }
      if (all[256] == 0) return false;  // no end-of-block code
      if (!build_huff(lit, all, nlen) || !build_huff(dist, all + nlen, ndist)) return false;
      if (!inflate_codes(bs, out, start, lit, dist)) return false;
    } else {
      return false;
    }
    if (bs.bad) return false;
    if (final_block) break;
  }
  if (consumed) *consumed = bs.pos;
  return true;
}

bool DecodeTexture(const uint8_t* data, size_t size, Image& out, std::string* error) {
  if (size < 2) {
    if (error) *error = "no data";
    return false;
  }
  const bool zlib = (data[0] >> 6) & 1;
  return zlib ? decode_zlib(data + 1, size - 1, out, error) : decode_bitpacked(data + 1, size - 1, out, error);
}

size_t DecodeAll(const std::vector<uint8_t>& rom, const RomVersion& version, std::vector<Image>& out, std::string* error) {
  out.clear();
  if (!version.texture_seg || size_t(version.texture_seg) + version.data_len > rom.size()) {
    if (error) *error = "this ROM version's texture table is not known";
    return 0;
  }
  const uint8_t* seg = rom.data() + version.texture_seg;
  const size_t table = size_t(version.texture_seg) + version.data_len;
  std::vector<uint32_t> offsets;
  for (size_t pos = table; pos + 8 <= rom.size(); pos += 8) {
    const uint32_t w0 = (rom[pos] << 24) | (rom[pos + 1] << 16) | (rom[pos + 2] << 8) | rom[pos + 3];
    const uint32_t w1 = (rom[pos + 4] << 24) | (rom[pos + 5] << 16) | (rom[pos + 6] << 8) | rom[pos + 7];
    if (w1 != 0) break;
    offsets.push_back(w0 & 0xFFFFFF);
    if (offsets.size() > 8192) break;
  }
  if (offsets.size() < 2) {
    if (error) *error = "no texture table";
    return 0;
  }
  const size_t count = offsets.size() - 1;  // the last entry ends the final texture's data
  out.resize(count);
  size_t failed = 0;
  std::string first_error;
  for (size_t id = 0; id < count; ++id) {
    const uint32_t a = offsets[id], b = offsets[id + 1];
    if (b <= a) continue;  // no data (id 3394)
    if (b > version.data_len) {
      if (error) *error = "texture " + std::to_string(id) + " runs past the data";
      out.clear();
      return 0;
    }
    std::string e;
    if (!DecodeTexture(seg + a, b - a, out[id], &e)) {
      out[id] = Image{};
      if (!failed++) first_error = "texture " + std::to_string(id) + ": " + e;
    }
  }
  if (failed && error) *error = std::to_string(failed) + " textures failed, first " + first_error;
  return count;
}

namespace {
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// A rarezip block (0x11 0x73, a 3-byte inflated size, raw deflate) at `p`.
bool rarezip(const uint8_t* p, size_t size, std::vector<uint8_t>& out) {
  if (size < 5 || p[0] != 0x11 || p[1] != 0x73) return false;
  out.clear();
  const size_t want = (size_t(p[2]) << 16) | (size_t(p[3]) << 8) | p[4];
  out.reserve(want);
  return Inflate(p + 5, size - 5, out) && out.size() == want;
}
}  // namespace

std::vector<uint32_t> FileOffsets(const std::vector<uint8_t>& rom, const RomVersion& version) {
  std::vector<uint32_t> offsets;
  if (!version.data_seg || version.data_seg >= rom.size()) return offsets;
  std::vector<uint8_t> data;
  if (!rarezip(rom.data() + version.data_seg, rom.size() - version.data_seg, data)) return offsets;
  for (size_t pos = version.files_table; pos + 4 <= data.size(); pos += 4) {
    const uint32_t v = be32(data.data() + pos);
    if (v == 0 && !offsets.empty()) break;
    if (v > rom.size() || offsets.size() > 4096) return {};
    offsets.push_back(v);
  }
  return offsets;
}

bool ExtractFile(const std::vector<uint8_t>& rom, const std::vector<uint32_t>& offsets, int id,
                 std::vector<uint8_t>& out) {
  if (id <= 0 || size_t(id) + 1 >= offsets.size()) return false;
  const uint32_t a = offsets[size_t(id)], b = offsets[size_t(id) + 1];
  if (b <= a || b > rom.size()) return false;
  const uint8_t* p = rom.data() + a;
  if (b - a >= 2 && p[0] == 0x11 && p[1] == 0x73) return rarezip(p, b - a, out);
  out.assign(p, p + (b - a));
  return true;
}

bool DecodeTmem(const uint8_t* data, size_t size, int fmt, int siz, int w, int h, Image& out) {
  // (G_IM_FMT, G_IM_SIZ) -> Image::format.
  int format = -1;
  if (fmt == 0 && siz == 2) format = 1;       // RGBA16
  else if (fmt == 0 && siz == 3) format = 0;  // RGBA32
  else if (fmt == 3 && siz == 0) format = 6;  // IA4
  else if (fmt == 3 && siz == 1) format = 5;  // IA8
  else if (fmt == 3 && siz == 2) format = 4;  // IA16
  else if (fmt == 4 && siz == 0) format = 8;  // I4
  else if (fmt == 4 && siz == 1) format = 7;  // I8
  if (format < 0 || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
  const int bits = 4 << siz;
  const size_t row = (size_t(w) * size_t(bits) / 8 + 7) & ~size_t(7);
  if (row * size_t(h) > size) return false;
  out.w = uint16_t(w);
  out.h = uint16_t(h);
  out.format = uint8_t(format);
  out.rgba.assign(size_t(w) * size_t(h) * 4, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* line = data + size_t(y) * row;
    const size_t swap = (y & 1) ? 4 : 0;  // odd rows: the 32-bit words of each 8 bytes swapped
    for (int x = 0; x < w; ++x) {
      const size_t bit = size_t(x) * size_t(bits);
      const size_t at = (bit / 8) ^ swap;
      uint32_t value = 0;
      switch (bits) {
        case 4: value = (line[at] >> ((bit & 4) ? 0 : 4)) & 15; break;
        case 8: value = line[at]; break;
        case 16: value = (uint32_t(line[at]) << 8) | line[at + 1]; break;
        default:
          value = (uint32_t(line[at]) << 24) | (uint32_t(line[at + 1]) << 16) | (uint32_t(line[at + 2]) << 8) |
                  line[at + 3];
          break;
      }
      put_rgba(format, value, out.rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4);
    }
  }
  return true;
}

}  // namespace pd::n64dec
