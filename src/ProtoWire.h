#pragma once

// A minimal protobuf wire reader, shared by the AirSense 11 readers (the
// Summary and the settings history). Internal to the library: not installed.
//
// Wire types 0 (varint), 1 (64-bit), 2 (length-delimited) and 5 (32-bit). The
// deprecated group types stop the message: nothing here is expected to carry
// one, and reading on past an unknown shape would misfile every later field.

#include <cstddef>
#include <cstdint>

namespace cpapdash::parser::protowire {

struct Field {
    uint32_t       number = 0;
    uint32_t       wire   = 0;
    uint64_t       varint = 0;        // wire 0, 1 and 5 (as the raw little-endian value)
    const uint8_t* bytes  = nullptr;  // wire 2
    size_t         len    = 0;
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    bool done() const { return i_ >= n_; }

    // The next field, or false at the end or at a shape that cannot be read.
    bool next(Field& f) {
        uint64_t key;
        if (!varint(key)) return false;
        f.number = static_cast<uint32_t>(key >> 3);
        f.wire   = static_cast<uint32_t>(key & 7);
        f.bytes  = nullptr;
        f.len    = 0;
        switch (f.wire) {
        case 0:
            return varint(f.varint);
        case 1:
            return fixed(8, f.varint);
        case 5:
            return fixed(4, f.varint);
        case 2: {
            uint64_t ln;
            if (!varint(ln) || ln > n_ - i_) return fail();
            f.bytes = d_ + i_;
            f.len   = static_cast<size_t>(ln);
            i_ += f.len;
            return true;
        }
        default:
            return fail();
        }
    }

private:
    bool fail() { i_ = n_; return false; }

    bool varint(uint64_t& out) {
        out = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (i_ >= n_) return fail();
            const uint8_t b = d_[i_++];
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return fail();   // more than ten bytes: not a varint
    }

    bool fixed(size_t width, uint64_t& out) {
        if (n_ - i_ < width) return fail();
        out = 0;
        for (size_t k = 0; k < width; k++) out |= static_cast<uint64_t>(d_[i_ + k]) << (8 * k);
        i_ += width;
        return true;
    }

    const uint8_t* d_;
    size_t         n_;
    size_t         i_ = 0;
};

}  // namespace cpapdash::parser::protowire
