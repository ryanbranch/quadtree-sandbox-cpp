// u256: 256-bit unsigned integer.
//
// Four little-endian 64-bit limbs (limbs[0] = LSB).
// Add/sub/cmp/shift are straightforward. Multiplication uses u128 partial
// products and wraps mod 2^256 (consistent with built-in unsigned overflow).
// Division uses shift-and-subtract long division (sufficient given how rarely
// divisor magnitude varies in this codebase).

#include "quadtree.h"
#include <cstdio>
#include <string>
#include <stdexcept>

bool operator==(const u256& a, const u256& b) {
    return a.limbs[0] == b.limbs[0] && a.limbs[1] == b.limbs[1]
        && a.limbs[2] == b.limbs[2] && a.limbs[3] == b.limbs[3];
}
bool operator!=(const u256& a, const u256& b) { return !(a == b); }

bool operator<(const u256& a, const u256& b) {
    for (int i = 3; i >= 0; i--) {
        if (a.limbs[i] != b.limbs[i]) return a.limbs[i] < b.limbs[i];
    }
    return false;
}
bool operator<=(const u256& a, const u256& b) { return !(b < a); }
bool operator>(const u256& a, const u256& b)  { return b < a; }
bool operator>=(const u256& a, const u256& b) { return !(a < b); }

u256 operator+(const u256& a, const u256& b) {
    u256 r;
    u128 carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 s = (u128)a.limbs[i] + (u128)b.limbs[i] + carry;
        r.limbs[i] = (uint64_t)s;
        carry = s >> 64;
    }
    return r;
}

u256 operator-(const u256& a, const u256& b) {
    u256 r;
    u128 borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 ai = a.limbs[i];
        u128 sub = (u128)b.limbs[i] + borrow;
        if (ai < sub) {
            r.limbs[i] = (uint64_t)((ai + ((u128)1 << 64)) - sub);
            borrow = 1;
        } else {
            r.limbs[i] = (uint64_t)(ai - sub);
            borrow = 0;
        }
    }
    return r;
}

u256 operator*(const u256& a, const u256& b) {
    // School multiplication mod 2^256. r[i+j] += a[i] * b[j].
    uint64_t r[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        u128 carry = 0;
        for (int j = 0; j + i < 4; j++) {
            u128 prod = (u128)a.limbs[i] * (u128)b.limbs[j] + r[i + j] + carry;
            r[i + j] = (uint64_t)prod;
            carry = prod >> 64;
        }
    }
    u256 out;
    for (int i = 0; i < 4; i++) out.limbs[i] = r[i];
    return out;
}

// Return true if v != 0.
static bool nonzero(const u256& v) {
    return v.limbs[0] | v.limbs[1] | v.limbs[2] | v.limbs[3];
}

// Bit length: position of highest set bit + 1, or 0 if v == 0.
static int bit_length(const u256& v) {
    for (int i = 3; i >= 0; i--) {
        if (v.limbs[i]) {
            int hi = 63;
            uint64_t x = v.limbs[i];
            while (!(x >> hi)) hi--;
            return i * 64 + hi + 1;
        }
    }
    return 0;
}

// Left-shift by n bits (n in 0..255). Wraps at 256 bits.
static u256 shl(const u256& v, int n) {
    if (n == 0) return v;
    u256 r;
    int word = n / 64;
    int bit  = n % 64;
    for (int i = 3; i >= 0; i--) {
        uint64_t hi = (i - word >= 0)     ? v.limbs[i - word]     : 0;
        uint64_t lo = (i - word - 1 >= 0 && bit) ? v.limbs[i - word - 1] : 0;
        if (bit == 0) r.limbs[i] = hi;
        else          r.limbs[i] = (hi << bit) | (lo >> (64 - bit));
    }
    return r;
}

// Divmod: returns quotient, sets *rem to remainder. Throws on divide-by-zero.
static u256 divmod_impl(const u256& a, const u256& b, u256* rem) {
    if (!nonzero(b)) throw std::runtime_error("u256 division by zero");

    // Fast path: if a < b, quotient is 0 and remainder is a.
    if (a < b) {
        if (rem) *rem = a;
        return u256();
    }

    // Shift-and-subtract: align b with the highest bit of a, then loop.
    int shift = bit_length(a) - bit_length(b);
    u256 cur = a;
    u256 d   = shl(b, shift);
    u256 q;

    for (int s = shift; s >= 0; s--) {
        if (d <= cur) {
            cur = cur - d;
            int word = s / 64;
            int bit  = s % 64;
            q.limbs[word] |= (uint64_t)1 << bit;
        }
        // shift d right by 1
        for (int i = 0; i < 3; i++) {
            d.limbs[i] = (d.limbs[i] >> 1) | (d.limbs[i + 1] << 63);
        }
        d.limbs[3] >>= 1;
    }

    if (rem) *rem = cur;
    return q;
}

u256 operator/(const u256& a, const u256& b) {
    return divmod_impl(a, b, nullptr);
}

u256 operator%(const u256& a, const u256& b) {
    u256 r;
    divmod_impl(a, b, &r);
    return r;
}

u256& operator++(u256& a) {
    a = a + u256((uint64_t)1);
    return a;
}
u256 operator++(u256& a, int) {
    u256 old = a;
    ++a;
    return old;
}

u256& operator+=(u256& a, const u256& b) { a = a + b; return a; }
u256& operator-=(u256& a, const u256& b) { a = a - b; return a; }
u256& operator*=(u256& a, const u256& b) { a = a * b; return a; }
u256& operator/=(u256& a, const u256& b) { a = a / b; return a; }
u256& operator%=(u256& a, const u256& b) { a = a % b; return a; }

std::string u256_to_string(const u256& v) {
    if (!nonzero(v)) return "0";
    u256 cur = v;
    u256 ten = (u256)(uint64_t)10;
    std::string s;
    while (nonzero(cur)) {
        u256 r;
        cur = divmod_impl(cur, ten, &r);
        s += (char)('0' + (int)r.limbs[0]);
    }
    std::string out(s.rbegin(), s.rend());
    return out;
}

void u256_print(const u256& v) {
    auto s = u256_to_string(v);
    fputs(s.c_str(), stdout);
}
