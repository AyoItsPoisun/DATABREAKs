
#pragma once

// DATABREAKs Engine 029 - portable exact 128-bit integer (host-side C++17).
//
// MusicalClock (Engine 025) needs exact 128-bit integer arithmetic: sample
// times are kept as integers scaled by milliBPM x 960, and long sessions
// exceed 64 bits. Engine 025 used the GCC/Clang extension __int128, which
// exists only on 64-bit targets: on a 32-bit microcontroller (e.g. ARM
// Cortex-M) the core did not compile at all. This was the only compile
// error when the whole core was type-checked for a 32-bit target.
//
// PortableInt128 is a small two's-complement integer made of two 64-bit
// words, with exactly the C++ semantics of __int128 for the operations the
// clock uses: +, - (binary and unary), * (modulo 2^128), / and % (truncate
// toward zero, remainder has the dividend's sign), comparisons, and
// conversion from / truncation to int64. Only standard C++17 is used.
//
// ExactInt128 is the type the clock uses:
//   * the native __int128 when the compiler provides it (64-bit hosts),
//     so host behaviour and speed are unchanged;
//   * PortableInt128 otherwise, or when DATABREAKS_PORTABLE_INT128 is
//     defined (used to run the whole test suite on the portable type and
//     prove it bit-identical; engine_029 also compares the two directly).
//
// No allocation, no exceptions, no platform headers. Division by zero is
// undefined, as for built-in integers (the clock never divides by zero).

#include <cstdint>

namespace databreaks {

class PortableInt128 {
public:
    constexpr PortableInt128() = default;
    // Implicit from 64-bit integers (and narrower ones via promotion), like
    // a built-in integer type.
    constexpr PortableInt128(std::int64_t v)  // NOLINT(google-explicit-constructor)
        : lo_(static_cast<std::uint64_t>(v)), hi_(v < 0 ? ~std::uint64_t{0} : 0) {}
    constexpr PortableInt128(int v) : PortableInt128(static_cast<std::int64_t>(v)) {}

    static constexpr PortableInt128 fromWords(std::uint64_t hi, std::uint64_t lo) {
        PortableInt128 r;
        r.hi_ = hi;
        r.lo_ = lo;
        return r;
    }
    constexpr std::uint64_t high() const { return hi_; }
    constexpr std::uint64_t low() const { return lo_; }

    // Truncates to the low 64 bits, like static_cast<std::int64_t>(__int128).
    explicit constexpr operator std::int64_t() const { return static_cast<std::int64_t>(lo_); }

    constexpr bool negative() const { return (hi_ >> 63) != 0; }

    friend constexpr PortableInt128 operator+(PortableInt128 a, PortableInt128 b) {
        const std::uint64_t lo = a.lo_ + b.lo_;
        return fromWords(a.hi_ + b.hi_ + (lo < a.lo_ ? 1u : 0u), lo);
    }
    friend constexpr PortableInt128 operator-(PortableInt128 a) {
        const std::uint64_t lo = ~a.lo_ + 1;
        return fromWords(~a.hi_ + (lo == 0 ? 1u : 0u), lo);
    }
    friend constexpr PortableInt128 operator-(PortableInt128 a, PortableInt128 b) {
        return a + (-b);
    }
    constexpr PortableInt128& operator--() { return *this = *this - PortableInt128(1); }
    constexpr PortableInt128& operator++() { return *this = *this + PortableInt128(1); }
    friend constexpr PortableInt128 operator*(PortableInt128 a, PortableInt128 b) {
        std::uint64_t hi = 0;
        const std::uint64_t lo = mul64(a.lo_, b.lo_, hi);
        hi += a.lo_ * b.hi_ + a.hi_ * b.lo_;  // modulo 2^64: the rest overflows
        return fromWords(hi, lo);
    }
    friend constexpr PortableInt128 operator/(PortableInt128 a, PortableInt128 b) {
        PortableInt128 q;
        PortableInt128 r;
        divmod(a, b, q, r);
        return q;
    }
    friend constexpr PortableInt128 operator%(PortableInt128 a, PortableInt128 b) {
        PortableInt128 q;
        PortableInt128 r;
        divmod(a, b, q, r);
        return r;
    }

    friend constexpr bool operator==(PortableInt128 a, PortableInt128 b) {
        return a.hi_ == b.hi_ && a.lo_ == b.lo_;
    }
    friend constexpr bool operator!=(PortableInt128 a, PortableInt128 b) { return !(a == b); }
    friend constexpr bool operator<(PortableInt128 a, PortableInt128 b) {
        const std::int64_t ah = static_cast<std::int64_t>(a.hi_);
        const std::int64_t bh = static_cast<std::int64_t>(b.hi_);
        return ah != bh ? ah < bh : a.lo_ < b.lo_;
    }
    friend constexpr bool operator>(PortableInt128 a, PortableInt128 b) { return b < a; }
    friend constexpr bool operator<=(PortableInt128 a, PortableInt128 b) { return !(b < a); }
    friend constexpr bool operator>=(PortableInt128 a, PortableInt128 b) { return !(a < b); }

private:
    std::uint64_t lo_ = 0;
    std::uint64_t hi_ = 0;

    // Full 64 x 64 -> 128 unsigned product from 32-bit halves.
    static constexpr std::uint64_t mul64(std::uint64_t x, std::uint64_t y, std::uint64_t& hi) {
        const std::uint64_t x0 = x & 0xFFFFFFFFu;
        const std::uint64_t x1 = x >> 32;
        const std::uint64_t y0 = y & 0xFFFFFFFFu;
        const std::uint64_t y1 = y >> 32;
        const std::uint64_t p00 = x0 * y0;
        const std::uint64_t p01 = x0 * y1;
        const std::uint64_t p10 = x1 * y0;
        const std::uint64_t p11 = x1 * y1;
        const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        return (p00 & 0xFFFFFFFFu) | (mid << 32);
    }

    // True when the value fits in int64 (hi is the sign extension of lo).
    constexpr bool fits64() const {
        return hi_ == (static_cast<std::int64_t>(lo_) < 0 ? ~std::uint64_t{0} : 0);
    }

    // Truncating signed division, C++ semantics.
    static constexpr void divmod(PortableInt128 a, PortableInt128 b, PortableInt128& q,
                                 PortableInt128& r) {
        // Fast path: both operands fit in 64 bits (the common case for the
        // clock); INT64_MIN / -1 is excluded because it overflows int64.
        if (a.fits64() && b.fits64()) {
            const std::int64_t x = static_cast<std::int64_t>(a.lo_);
            const std::int64_t y = static_cast<std::int64_t>(b.lo_);
            if (!(x == INT64_MIN && y == -1)) {
                q = PortableInt128(x / y);
                r = PortableInt128(x % y);
                return;
            }
        }
        const bool na = a.negative();
        const bool nb = b.negative();
        const PortableInt128 ua = na ? -a : a;  // magnitudes (2^127 stays 2^127 unsigned)
        const PortableInt128 ub = nb ? -b : b;
        std::uint64_t qh = 0, ql = 0, rh = 0, rl = 0;
        udivmod(ua.hi_, ua.lo_, ub.hi_, ub.lo_, qh, ql, rh, rl);
        q = fromWords(qh, ql);
        r = fromWords(rh, rl);
        if (na != nb) q = -q;
        if (na) r = -r;
    }

    // Unsigned 128 / 128 restoring division over the dividend's significant
    // bits (at most 128 iterations; ~70 for the clock's values).
    static constexpr void udivmod(std::uint64_t nh, std::uint64_t nl, std::uint64_t dh,
                                  std::uint64_t dl, std::uint64_t& qh, std::uint64_t& ql,
                                  std::uint64_t& rh, std::uint64_t& rl) {
        qh = ql = rh = rl = 0;
        int bits = 128;
        while (bits > 0 && bit(nh, nl, bits - 1) == 0) --bits;
        for (int i = bits - 1; i >= 0; --i) {
            // r = (r << 1) | bit i of n
            rh = (rh << 1) | (rl >> 63);
            rl = (rl << 1) | bit(nh, nl, i);
            if (rh > dh || (rh == dh && rl >= dl)) {
                const std::uint64_t borrow = rl < dl ? 1u : 0u;
                rl -= dl;
                rh -= dh + borrow;
                if (i >= 64) {
                    qh |= std::uint64_t{1} << (i - 64);
                } else {
                    ql |= std::uint64_t{1} << i;
                }
            }
        }
    }

    static constexpr std::uint64_t bit(std::uint64_t h, std::uint64_t l, int i) {
        return i >= 64 ? (h >> (i - 64)) & 1u : (l >> i) & 1u;
    }
};

#if defined(__SIZEOF_INT128__) && !defined(DATABREAKS_PORTABLE_INT128)
__extension__ typedef __int128 ExactInt128;
constexpr bool ExactInt128IsNative = true;
#else
using ExactInt128 = PortableInt128;
constexpr bool ExactInt128IsNative = false;
#endif

} // namespace databreaks
