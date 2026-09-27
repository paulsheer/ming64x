/* Copyright (C) 2020  Paul Sheer, All rights reserved. */

/* Supports curve25519 in pure C for 32-bit and 64-bit builds. */


/* Copyright 2008, Google Inc.
 * All rights reserved.
 *
 * Code released into the public domain.
 *
 * curve25519-donna: Curve25519 elliptic curve, public key function
 *
 * http://code.google.com/p/curve25519-donna/
 *
 * Adam Langley <agl@imperialviolet.org>
 *
 * Derived from public domain C code by Daniel J. Bernstein <djb@cr.yp.to>
 *
 * More information about curve25519 can be found here
 *   http://cr.yp.to/ecdh.html
 *
 * djb's sample implementation of curve25519 is written in a special assembly
 * language called qhasm and uses the floating point registers.
 *
 * This is, almost, a clean room reimplementation from the curve25519 paper. It
 * uses many of the tricks described therein. Only the crecip function is taken
 * from the sample implementation.
 */

#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint64_t limb;
typedef limb felem[5];

#include "math128.h"


#undef force_inline
#define force_inline __attribute__((always_inline))

/* Sum two numbers: output += in */
static inline void force_inline
fsum(limb *output, const limb *in) {
  output[0] += in[0];
  output[1] += in[1];
  output[2] += in[2];
  output[3] += in[3];
  output[4] += in[4];
}

/* Find the difference of two numbers: output = in - output
 * (note the order of the arguments!)
 *
 * Assumes that out[i] < 2**52
 * On return, out[i] < 2**55
 */
static inline void force_inline
fdifference_backwards(felem out, const felem in) {
  /* 152 is 19 << 3 */
  static const limb two54m152 = (((limb)1) << 54) - 152;
  static const limb two54m8 = (((limb)1) << 54) - 8;

  out[0] = in[0] + two54m152 - out[0];
  out[1] = in[1] + two54m8 - out[1];
  out[2] = in[2] + two54m8 - out[2];
  out[3] = in[3] + two54m8 - out[3];
  out[4] = in[4] + two54m8 - out[4];
}

/* Multiply a number by a scalar: output = in * scalar */
static inline void force_inline fscalar_product (felem output, const felem in, const limb scalar)
{
    uint128_t a, b;

    u128mul (a, in[0], scalar);
    output[0] = u128lo (a) & 0x7ffffffffffff;
    b = a;

    u128mul (a, in[1], scalar);
    u128addi (a, _u128shft (b, 51));
    output[1] = u128lo (a) & 0x7ffffffffffff;
    b = a;

    u128mul (a, in[2], scalar);
    u128addi (a, _u128shft (b, 51));
    output[2] = u128lo (a) & 0x7ffffffffffff;
    b = a;

    u128mul (a, in[3], scalar);
    u128addi (a, _u128shft (b, 51));
    output[3] = u128lo (a) & 0x7ffffffffffff;
    b = a;

    u128mul (a, in[4], scalar);
    u128addi (a, _u128shft (b, 51));
    output[4] = u128lo (a) & 0x7ffffffffffff;

    output[0] += _u128shft (a, 51) * 19;
}

/* Multiply two numbers: output = in2 * in
 *
 * output must be distinct to both inputs. The inputs are reduced coefficient
 * form, the output is not.
 *
 * Assumes that in[i] < 2**55 and likewise for in2.
 * On return, output[i] < 2**52
 */
static inline void force_inline fmul (felem output, const felem in2, const felem in)
{
    uint128_t t[5];
    limb r0, r1, r2, r3, r4, s0, s1, s2, s3, s4, c;

    r0 = in[0];
    r1 = in[1];
    r2 = in[2];
    r3 = in[3];
    r4 = in[4];

    s0 = in2[0];
    s1 = in2[1];
    s2 = in2[2];
    s3 = in2[3];
    s4 = in2[4];

    u128mul (t[0], r0, s0);

    u128mul (t[1], r0, s1);
    u128muladd (t[1], r1, s0);

    u128mul (t[2], r0, s2);
    u128muladd (t[2], r2, s0);
    u128muladd (t[2], r1, s1);

    u128mul (t[3], r0, s3);
    u128muladd (t[3], r3, s0);
    u128muladd (t[3], r1, s2);
    u128muladd (t[3], r2, s1);

    u128mul (t[4], r0, s4);
    u128muladd (t[4], r4, s0);
    u128muladd (t[4], r1, s3);
    u128muladd (t[4], r3, s1);
    u128muladd (t[4], r2, s2);

    r4 *= 19;
    r1 *= 19;
    r2 *= 19;
    r3 *= 19;

    u128muladd (t[0], r4, s1);
    u128muladd (t[0], r1, s4);
    u128muladd (t[0], r2, s3);
    u128muladd (t[0], r3, s2);

    u128muladd (t[1], r4, s2);
    u128muladd (t[1], r2, s4);
    u128muladd (t[1], r3, s3);

    u128muladd (t[2], r4, s3);
    u128muladd (t[2], r3, s4);

    u128muladd (t[3], r4, s4);

    r0 = u128lo (t[0]) & 0x7ffffffffffff;
    u128addi (t[1], _u128shft (t[0], 51));
    r1 = u128lo (t[1]) & 0x7ffffffffffff;
    u128addi (t[2], _u128shft (t[1], 51));
    r2 = u128lo (t[2]) & 0x7ffffffffffff;
    u128addi (t[3], _u128shft (t[2], 51));
    r3 = u128lo (t[3]) & 0x7ffffffffffff;
    u128addi (t[4], _u128shft (t[3], 51));
    r4 = u128lo (t[4]) & 0x7ffffffffffff;
    r0 += _u128shft (t[4], 51) * 19;
    c = r0 >> 51;
    r0 &= 0x7ffffffffffff;
    r1 += c;
    c = r1 >> 51;
    r1 &= 0x7ffffffffffff;
    r2 += c;

    output[0] = r0;
    output[1] = r1;
    output[2] = r2;
    output[3] = r3;
    output[4] = r4;
}

static inline void force_inline
fsquare_times(felem output, const felem in, limb count) {
  uint128_t t[5], w;
  limb r0,r1,r2,r3,r4,c;
  limb d0,d1,d2,d4,d419;

  r0 = in[0];
  r1 = in[1];
  r2 = in[2];
  r3 = in[3];
  r4 = in[4];

  do {
    d0 = r0 * 2;
    d1 = r1 * 2;
    d2 = r2 * 2 * 19;
    d419 = r4 * 19;
    d4 = d419 * 2;

    u128mul (t[0], r0, r0);
    u128muladd (t[0], d4, r1);
    u128muladd (t[0], d2, r3);

    u128mul (t[1], d0, r1);
    u128muladd (t[1], d4, r2);
    u128muladd (t[1], r3, r3 * 19);

    u128mul (t[2], d0, r2);
    u128muladd (t[2], r1, r1);
    u128muladd (t[2], d4, r3);

    u128mul (t[3], d0, r3);
    u128muladd (t[3], d1, r2);
    u128muladd (t[3], r4, d419);

    u128mul (t[4], d0, r4);
    u128muladd (t[4], d1, r3);
    u128muladd (t[4], r2, r2);

                        r0 = u128lo (t[0]) & 0x7ffffffffffff; w = t[0]; u128shft (w, 51); c = u128lo (w);
    u128addi (t[1], c); r1 = u128lo (t[1]) & 0x7ffffffffffff; w = t[1]; u128shft (w, 51); c = u128lo (w);
    u128addi (t[2], c); r2 = u128lo (t[2]) & 0x7ffffffffffff; w = t[2]; u128shft (w, 51); c = u128lo (w);
    u128addi (t[3], c); r3 = u128lo (t[3]) & 0x7ffffffffffff; w = t[3]; u128shft (w, 51); c = u128lo (w);
    u128addi (t[4], c); r4 = u128lo (t[4]) & 0x7ffffffffffff; w = t[4]; u128shft (w, 51); c = u128lo (w);
    r0 +=   c * 19; c = r0 >> 51; r0 = r0 & 0x7ffffffffffff;
    r1 +=   c;      c = r1 >> 51; r1 = r1 & 0x7ffffffffffff;
    r2 +=   c;
  } while(--count);

  output[0] = r0;
  output[1] = r1;
  output[2] = r2;
  output[3] = r3;
  output[4] = r4;
}

/* Load a little-endian 64-bit number  */
static limb
load_limb(const u8 *in) {
  return
    ((limb)in[0]) |
    (((limb)in[1]) << 8) |
    (((limb)in[2]) << 16) |
    (((limb)in[3]) << 24) |
    (((limb)in[4]) << 32) |
    (((limb)in[5]) << 40) |
    (((limb)in[6]) << 48) |
    (((limb)in[7]) << 56);
}

static void
store_limb(u8 *out, limb in) {
  out[0] = in & 0xff;
  out[1] = (in >> 8) & 0xff;
  out[2] = (in >> 16) & 0xff;
  out[3] = (in >> 24) & 0xff;
  out[4] = (in >> 32) & 0xff;
  out[5] = (in >> 40) & 0xff;
  out[6] = (in >> 48) & 0xff;
  out[7] = (in >> 56) & 0xff;
}

/* Take a little-endian, 32-byte number and expand it into polynomial form */
static void
fexpand(limb *output, const u8 *in) {
  output[0] = load_limb(in) & 0x7ffffffffffff;
  output[1] = (load_limb(in+6) >> 3) & 0x7ffffffffffff;
  output[2] = (load_limb(in+12) >> 6) & 0x7ffffffffffff;
  output[3] = (load_limb(in+19) >> 1) & 0x7ffffffffffff;
  output[4] = (load_limb(in+24) >> 12) & 0x7ffffffffffff;
}

/* Take a fully reduced polynomial form number and contract it into a
 * little-endian, 32-byte array
 */
static void
fcontract(u8 *output, const felem input) {
    uint128_t t[5];

    u128seti (t[0], input[0]);
    u128seti (t[1], input[1]);
    u128seti (t[2], input[2]);
    u128seti (t[3], input[3]);
    u128seti (t[4], input[4]);

    uint128_t b;

    b = t[0]; u128shft (b, 51); u128add (t[1], b); u128seti (t[0], u128lo (t[0]) & 0x7ffffffffffff);
    b = t[1]; u128shft (b, 51); u128add (t[2], b); u128seti (t[1], u128lo (t[1]) & 0x7ffffffffffff);
    b = t[2]; u128shft (b, 51); u128add (t[3], b); u128seti (t[2], u128lo (t[2]) & 0x7ffffffffffff);
    b = t[3]; u128shft (b, 51); u128add (t[4], b); u128seti (t[3], u128lo (t[3]) & 0x7ffffffffffff);
    b = t[4]; u128shft (b, 51); u128mul32 (b, 19);
                                u128add (t[0], b); u128seti (t[4], u128lo (t[4]) & 0x7ffffffffffff);

    b = t[0]; u128shft (b, 51); u128add (t[1], b); u128seti (t[0], u128lo (t[0]) & 0x7ffffffffffff);
    b = t[1]; u128shft (b, 51); u128add (t[2], b); u128seti (t[1], u128lo (t[1]) & 0x7ffffffffffff);
    b = t[2]; u128shft (b, 51); u128add (t[3], b); u128seti (t[2], u128lo (t[2]) & 0x7ffffffffffff);
    b = t[3]; u128shft (b, 51); u128add (t[4], b); u128seti (t[3], u128lo (t[3]) & 0x7ffffffffffff);
    b = t[4]; u128shft (b, 51); u128mul32 (b, 19);
                                u128add (t[0], b); u128seti (t[4], u128lo (t[4]) & 0x7ffffffffffff);

    /* now t is between 0 and 2^255-1, properly carried. */
    /* case 1: between 0 and 2^255-20. case 2: between 2^255-19 and 2^255-1. */

    u128addi (t[0], 19);

    b = t[0]; u128shft (b, 51); u128add (t[1], b); u128seti (t[0], u128lo (t[0]) & 0x7ffffffffffff);
    b = t[1]; u128shft (b, 51); u128add (t[2], b); u128seti (t[1], u128lo (t[1]) & 0x7ffffffffffff);
    b = t[2]; u128shft (b, 51); u128add (t[3], b); u128seti (t[2], u128lo (t[2]) & 0x7ffffffffffff);
    b = t[3]; u128shft (b, 51); u128add (t[4], b); u128seti (t[3], u128lo (t[3]) & 0x7ffffffffffff);
    b = t[4]; u128shft (b, 51); u128mul32 (b, 19);
                                u128add (t[0], b); u128seti (t[4], u128lo (t[4]) & 0x7ffffffffffff);

    /* now between 19 and 2^255-1 in both cases, and offset by 19. */
    u128addi (t[0], 0x8000000000000 - 19);
    u128addi (t[1], 0x8000000000000 - 1);
    u128addi (t[2], 0x8000000000000 - 1);
    u128addi (t[3], 0x8000000000000 - 1);
    u128addi (t[4], 0x8000000000000 - 1);

    /* now between 2^255 and 2^256-20, and offset by 2^255. */

    b = t[0]; u128shft (b, 51); u128add (t[1], b); u128seti (t[0], u128lo (t[0]) & 0x7ffffffffffff);
    b = t[1]; u128shft (b, 51); u128add (t[2], b); u128seti (t[1], u128lo (t[1]) & 0x7ffffffffffff);
    b = t[2]; u128shft (b, 51); u128add (t[3], b); u128seti (t[2], u128lo (t[2]) & 0x7ffffffffffff);
    b = t[3]; u128shft (b, 51); u128add (t[4], b); u128seti (t[3], u128lo (t[3]) & 0x7ffffffffffff);
    
    u128seti (t[4], u128lo (t[4]) & 0x7ffffffffffff);

    store_limb(output,    u128lo (t[0]) | (u128lo (t[1]) << 51));
    store_limb(output+8,  (u128lo (t[1]) >> 13) | (u128lo (t[2]) << 38));
    store_limb(output+16, (u128lo (t[2]) >> 26) | (u128lo (t[3]) << 25));
    store_limb(output+24, (u128lo (t[3]) >> 39) | (u128lo (t[4]) << 12));
}

/* Input: Q, Q', Q-Q'
 * Output: 2Q, Q+Q'
 *
 *   x2 z3: long form
 *   x3 z3: long form
 *   x z: short form, destroyed
 *   xprime zprime: short form, destroyed
 *   qmqp: short form, preserved
 */
static void
fmonty(limb *x2, limb *z2, /* output 2Q */
       limb *x3, limb *z3, /* output Q + Q' */
       limb *x, limb *z,   /* input Q */
       limb *xprime, limb *zprime, /* input Q' */
       const limb *qmqp /* input Q - Q' */) {
  limb origx[5], origxprime[5], zzz[5], xx[5], zz[5], xxprime[5],
        zzprime[5], zzzprime[5];

  memcpy(origx, x, 5 * sizeof(limb));
  fsum(x, z);
  fdifference_backwards(z, origx);  // does x - z

  memcpy(origxprime, xprime, sizeof(limb) * 5);
  fsum(xprime, zprime);
  fdifference_backwards(zprime, origxprime);
  fmul(xxprime, xprime, z);
  fmul(zzprime, x, zprime);
  memcpy(origxprime, xxprime, sizeof(limb) * 5);
  fsum(xxprime, zzprime);
  fdifference_backwards(zzprime, origxprime);
  fsquare_times(x3, xxprime, 1);
  fsquare_times(zzzprime, zzprime, 1);
  fmul(z3, zzzprime, qmqp);

  fsquare_times(xx, x, 1);
  fsquare_times(zz, z, 1);
  fmul(x2, xx, zz);
  fdifference_backwards(zz, xx);  // does zz = xx - zz
  fscalar_product(zzz, zz, 121665);
  fsum(zzz, xx);
  fmul(z2, zz, zzz);
}

// -----------------------------------------------------------------------------
// Maybe swap the contents of two limb arrays (@a and @b), each @len elements
// long. Perform the swap iff @swap is non-zero.
//
// This function performs the swap without leaking any side-channel
// information.
// -----------------------------------------------------------------------------
static void
swap_conditional(limb a[5], limb b[5], limb iswap) {
  unsigned i;
  const limb swap = -iswap;

  for (i = 0; i < 5; ++i) {
    const limb x = swap & (a[i] ^ b[i]);
    a[i] ^= x;
    b[i] ^= x;
  }
}

/* Calculates nQ where Q is the x-coordinate of a point on the curve
 *
 *   resultx/resultz: the x coordinate of the resulting curve point (short form)
 *   n: a little endian, 32-byte number
 *   q: a point of the curve (short form)
 */
static void
cmult(limb *resultx, limb *resultz, const u8 *n, const limb *q) {
  limb a[5] = {0}, b[5] = {1}, c[5] = {1}, d[5] = {0};
  limb *nqpqx = a, *nqpqz = b, *nqx = c, *nqz = d, *t;
  limb e[5] = {0}, f[5] = {1}, g[5] = {0}, h[5] = {1};
  limb *nqpqx2 = e, *nqpqz2 = f, *nqx2 = g, *nqz2 = h;

  unsigned i, j;

  memcpy(nqpqx, q, sizeof(limb) * 5);

  for (i = 0; i < 32; ++i) {
    u8 byte = n[31 - i];
    for (j = 0; j < 8; ++j) {
      const limb bit = byte >> 7;

      swap_conditional(nqx, nqpqx, bit);
      swap_conditional(nqz, nqpqz, bit);
      fmonty(nqx2, nqz2,
             nqpqx2, nqpqz2,
             nqx, nqz,
             nqpqx, nqpqz,
             q);
      swap_conditional(nqx2, nqpqx2, bit);
      swap_conditional(nqz2, nqpqz2, bit);

      t = nqx;
      nqx = nqx2;
      nqx2 = t;
      t = nqz;
      nqz = nqz2;
      nqz2 = t;
      t = nqpqx;
      nqpqx = nqpqx2;
      nqpqx2 = t;
      t = nqpqz;
      nqpqz = nqpqz2;
      nqpqz2 = t;

      byte <<= 1;
    }
  }

  memcpy(resultx, nqx, sizeof(limb) * 5);
  memcpy(resultz, nqz, sizeof(limb) * 5);
}


// -----------------------------------------------------------------------------
// Shamelessly copied from djb's code, tightened a little
// -----------------------------------------------------------------------------
static void
crecip(felem out, const felem z) {
  felem a,t0,b,c;

  /* 2 */ fsquare_times(a, z, 1); // a = 2
  /* 8 */ fsquare_times(t0, a, 2);
  /* 9 */ fmul(b, t0, z); // b = 9
  /* 11 */ fmul(a, b, a); // a = 11
  /* 22 */ fsquare_times(t0, a, 1);
  /* 2^5 - 2^0 = 31 */ fmul(b, t0, b);
  /* 2^10 - 2^5 */ fsquare_times(t0, b, 5);
  /* 2^10 - 2^0 */ fmul(b, t0, b);
  /* 2^20 - 2^10 */ fsquare_times(t0, b, 10);
  /* 2^20 - 2^0 */ fmul(c, t0, b);
  /* 2^40 - 2^20 */ fsquare_times(t0, c, 20);
  /* 2^40 - 2^0 */ fmul(t0, t0, c);
  /* 2^50 - 2^10 */ fsquare_times(t0, t0, 10);
  /* 2^50 - 2^0 */ fmul(b, t0, b);
  /* 2^100 - 2^50 */ fsquare_times(t0, b, 50);
  /* 2^100 - 2^0 */ fmul(c, t0, b);
  /* 2^200 - 2^100 */ fsquare_times(t0, c, 100);
  /* 2^200 - 2^0 */ fmul(t0, t0, c);
  /* 2^250 - 2^50 */ fsquare_times(t0, t0, 50);
  /* 2^250 - 2^0 */ fmul(t0, t0, b);
  /* 2^255 - 2^5 */ fsquare_times(t0, t0, 5);
  /* 2^255 - 21 */ fmul(out, t0, a);
}

int
curve25519(u8 *mypublic, const u8 *secret, const u8 *basepoint) {
  limb bp[5], x[5], z[5], zmone[5];
  uint8_t e[32];
  int i;

  for (i = 0;i < 32;++i) e[i] = secret[i];
  e[0] &= 248;
  e[31] &= 127;
  e[31] |= 64;

  fexpand(bp, basepoint);
  cmult(x, z, e, bp);
  crecip(zmone, z);
  fmul(z, x, zmone);
  fcontract(mypublic, z);
  return 0;
}


/* ============================================================================
 * Ed25519 signatures (public domain; derived from TweetNaCl by Daniel J.
 * Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe,
 * Sjaak Smetsers).  Only the SHA-512 + Edwards group/signature primitives are
 * retained; the Salsa20/Poly1305/box/Montgomery code is omitted.
 * ========================================================================== */

#include <stdlib.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i64 gf[16];

#define FOR(i,n) for (i = 0; i < (n); ++i)
#define sv static void

static const gf
  gf0,
  gf1 = {1},
  D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
  D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
  X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
  Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
  I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static u64
dl64(const u8 *x)
{
  u64 i, u = 0;
  FOR(i, 8) u = (u << 8) | x[i];
  return u;
}

sv
ts64(u8 *x, u64 u)
{
  int i;
  for (i = 7; i >= 0; --i) { x[i] = u; u >>= 8; }
}

static int
vn(const u8 *x, const u8 *y, int n)
{
  u32 i, d = 0;
  FOR(i, n) d |= x[i] ^ y[i];
  return (1 & ((d - 1) >> 8)) - 1;
}

static int
crypto_verify_32(const u8 *x, const u8 *y)
{
  return vn(x, y, 32);
}

sv
set25519(gf r, const gf a)
{
  int i;
  FOR(i, 16) r[i] = a[i];
}

sv
car25519(gf o)
{
  int i;
  i64 c;
  FOR(i, 16) {
    o[i] += (1LL << 16);
    c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c << 16;
  }
}

sv
sel25519(gf p, gf q, int b)
{
  i64 t, i, c = ~(b - 1);
  FOR(i, 16) {
    t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

sv
pack25519(u8 *o, const gf n)
{
  int i, j, b;
  gf m, t;
  FOR(i, 16) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  FOR(j, 2) {
    m[0] = t[0] - 0xffed;
    for (i = 1; i < 15; i++) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    b = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  FOR(i, 16) {
    o[2 * i] = t[i] & 0xff;
    o[2 * i + 1] = t[i] >> 8;
  }
}

static int
neq25519(const gf a, const gf b)
{
  u8 c[32], d[32];
  pack25519(c, a);
  pack25519(d, b);
  return crypto_verify_32(c, d);
}

static u8
par25519(const gf a)
{
  u8 d[32];
  pack25519(d, a);
  return d[0] & 1;
}

sv
unpack25519(gf o, const u8 *n)
{
  int i;
  FOR(i, 16) o[i] = n[2 * i] + ((i64) n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

sv
A(gf o, const gf a, const gf b)
{
  int i;
  FOR(i, 16) o[i] = a[i] + b[i];
}

sv
Z(gf o, const gf a, const gf b)
{
  int i;
  FOR(i, 16) o[i] = a[i] - b[i];
}

sv
M(gf o, const gf a, const gf b)
{
  i64 i, j, t[31];
  FOR(i, 31) t[i] = 0;
  FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
  FOR(i, 15) t[i] += 38 * t[i + 16];
  FOR(i, 16) o[i] = t[i];
  car25519(o);
  car25519(o);
}

sv
S(gf o, const gf a)
{
  M(o, a, a);
}

sv
inv25519(gf o, const gf i)
{
  gf c;
  int a;
  FOR(a, 16) c[a] = i[a];
  for (a = 253; a >= 0; a--) {
    S(c, c);
    if (a != 2 && a != 4) M(c, c, i);
  }
  FOR(a, 16) o[a] = c[a];
}

sv
pow2523(gf o, const gf i)
{
  gf c;
  int a;
  FOR(a, 16) c[a] = i[a];
  for (a = 250; a >= 0; a--) {
    S(c, c);
    if (a != 1) M(c, c, i);
  }
  FOR(a, 16) o[a] = c[a];
}

static u64
R(u64 x, int c) { return (x >> c) | (x << (64 - c)); }

static u64
Ch(u64 x, u64 y, u64 z) { return (x & y) ^ (~x & z); }

static u64
Maj(u64 x, u64 y, u64 z) { return (x & y) ^ (x & z) ^ (y & z); }

static u64
Sigma0(u64 x) { return R(x, 28) ^ R(x, 34) ^ R(x, 39); }

static u64
Sigma1(u64 x) { return R(x, 14) ^ R(x, 18) ^ R(x, 41); }

static u64
sigma0(u64 x) { return R(x, 1) ^ R(x, 8) ^ (x >> 7); }

static u64
sigma1(u64 x) { return R(x, 19) ^ R(x, 61) ^ (x >> 6); }

static const u64 K[80] =
{
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static int
crypto_hashblocks(u8 *x, const u8 *m, u64 n)
{
  u64 z[8], b[8], a[8], w[16], t;
  int i, j;

  FOR(i, 8) z[i] = a[i] = dl64(x + 8 * i);

  while (n >= 128) {
    FOR(i, 16) w[i] = dl64(m + 8 * i);

    FOR(i, 80) {
      FOR(j, 8) b[j] = a[j];
      t = a[7] + Sigma1(a[4]) + Ch(a[4], a[5], a[6]) + K[i] + w[i % 16];
      b[7] = t + Sigma0(a[0]) + Maj(a[0], a[1], a[2]);
      b[3] += t;
      FOR(j, 8) a[(j + 1) % 8] = b[j];
      if (i % 16 == 15)
        FOR(j, 16)
          w[j] += w[(j + 9) % 16] + sigma0(w[(j + 1) % 16]) + sigma1(w[(j + 14) % 16]);
    }

    FOR(i, 8) { a[i] += z[i]; z[i] = a[i]; }

    m += 128;
    n -= 128;
  }

  FOR(i, 8) ts64(x + 8 * i, z[i]);

  return n;
}

static const u8 iv[64] = {
  0x6a,0x09,0xe6,0x67,0xf3,0xbc,0xc9,0x08,
  0xbb,0x67,0xae,0x85,0x84,0xca,0xa7,0x3b,
  0x3c,0x6e,0xf3,0x72,0xfe,0x94,0xf8,0x2b,
  0xa5,0x4f,0xf5,0x3a,0x5f,0x1d,0x36,0xf1,
  0x51,0x0e,0x52,0x7f,0xad,0xe6,0x82,0xd1,
  0x9b,0x05,0x68,0x8c,0x2b,0x3e,0x6c,0x1f,
  0x1f,0x83,0xd9,0xab,0xfb,0x41,0xbd,0x6b,
  0x5b,0xe0,0xcd,0x19,0x13,0x7e,0x21,0x79
};

static int
crypto_hash(u8 *out, const u8 *m, u64 n)
{
  u8 h[64], x[256];
  u64 i, b = n;

  FOR(i, 64) h[i] = iv[i];

  crypto_hashblocks(h, m, n);
  m += n;
  n &= 127;
  m -= n;

  FOR(i, 256) x[i] = 0;
  FOR(i, n) x[i] = m[i];
  x[n] = 128;

  n = 256 - 128 * (n < 112);
  x[n - 9] = b >> 61;
  ts64(x + n - 8, b << 3);
  crypto_hashblocks(h, x, n);

  FOR(i, 64) out[i] = h[i];

  return 0;
}

sv
add(gf p[4], gf q[4])
{
  gf a, b, c, d, t, e, f, g, h;

  Z(a, p[1], p[0]);
  Z(t, q[1], q[0]);
  M(a, a, t);
  A(b, p[0], p[1]);
  A(t, q[0], q[1]);
  M(b, b, t);
  M(c, p[3], q[3]);
  M(c, c, D2);
  M(d, p[2], q[2]);
  A(d, d, d);
  Z(e, b, a);
  Z(f, d, c);
  A(g, d, c);
  A(h, b, a);

  M(p[0], e, f);
  M(p[1], h, g);
  M(p[2], g, f);
  M(p[3], e, h);
}

sv
cswap(gf p[4], gf q[4], u8 b)
{
  int i;
  FOR(i, 4)
    sel25519(p[i], q[i], b);
}

sv
pack(u8 *r, gf p[4])
{
  gf tx, ty, zi;
  inv25519(zi, p[2]);
  M(tx, p[0], zi);
  M(ty, p[1], zi);
  pack25519(r, ty);
  r[31] ^= par25519(tx) << 7;
}

sv
scalarmult(gf p[4], gf q[4], const u8 *s)
{
  int i;
  set25519(p[0], gf0);
  set25519(p[1], gf1);
  set25519(p[2], gf1);
  set25519(p[3], gf0);
  for (i = 255; i >= 0; --i) {
    u8 b = (s[i / 8] >> (i & 7)) & 1;
    cswap(p, q, b);
    add(q, p);
    add(p, p);
    cswap(p, q, b);
  }
}

sv
scalarbase(gf p[4], const u8 *s)
{
  gf q[4];
  set25519(q[0], X);
  set25519(q[1], Y);
  set25519(q[2], gf1);
  M(q[3], X, Y);
  scalarmult(p, q, s);
}

static const u64 L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

sv
modL(u8 *r, i64 x[64])
{
  i64 carry, i, j;
  for (i = 63; i >= 32; --i) {
    carry = 0;
    for (j = i - 32; j < i - 12; ++j) {
      x[j] += carry - 16 * x[i] * L[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry << 8;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  FOR(j, 32) {
    x[j] += carry - (x[31] >> 4) * L[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  FOR(j, 32) x[j] -= carry * L[j];
  FOR(i, 32) {
    x[i + 1] += x[i] >> 8;
    r[i] = x[i] & 255;
  }
}

sv
reduce(u8 *r)
{
  i64 x[64], i;
  FOR(i, 64) x[i] = (u64) r[i];
  FOR(i, 64) r[i] = 0;
  modL(r, x);
}

static int
unpackneg(gf r[4], const u8 p[32])
{
  gf t, chk, num, den, den2, den4, den6;
  set25519(r[2], gf1);
  unpack25519(r[1], p);
  S(num, r[1]);
  M(den, num, D);
  Z(num, num, r[2]);
  A(den, r[2], den);

  S(den2, den);
  S(den4, den2);
  M(den6, den4, den2);
  M(t, den6, num);
  M(t, t, den);

  pow2523(t, t);
  M(t, t, num);
  M(t, t, den);
  M(t, t, den);
  M(r[0], t, den);

  S(chk, r[0]);
  M(chk, chk, den);
  if (neq25519(chk, num)) M(r[0], r[0], I);

  S(chk, r[0]);
  M(chk, chk, den);
  if (neq25519(chk, num)) return -1;

  if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);

  M(r[3], r[0], r[1]);
  return 0;
}

/* Derive the 32-byte public key from a 32-byte secret seed. */
int
ed25519_publickey(const u8 *seed, u8 *pk)
{
  u8 d[64];
  gf p[4];

  crypto_hash(d, seed, 32);
  d[0] &= 248;
  d[31] &= 127;
  d[31] |= 64;

  scalarbase(p, d);
  pack(pk, p);
  return 0;
}

/* Sign m (mlen bytes) with seed (32-byte secret) + pk (32-byte public);
 * writes the 64-byte R||S signature to sig.  Returns 0, or -1 on OOM. */
int
ed25519_sign(u8 *sig, const u8 *m, size_t mlen, const u8 *seed, const u8 *pk)
{
  u8 *sm;
  u8 d[64], h[64], r[64];
  i64 i, j, x[64];
  gf p[4];

  sm = (u8 *) malloc(mlen + 64);
  if (!sm)
    return -1;

  crypto_hash(d, seed, 32);
  d[0] &= 248;
  d[31] &= 127;
  d[31] |= 64;

  FOR(i, (i64) mlen) sm[64 + i] = m[i];
  FOR(i, 32) sm[32 + i] = d[32 + i];

  crypto_hash(r, sm + 32, mlen + 32);
  reduce(r);
  scalarbase(p, r);
  pack(sm, p);

  FOR(i, 32) sm[i + 32] = pk[i];
  crypto_hash(h, sm, mlen + 64);
  reduce(h);

  FOR(i, 64) x[i] = 0;
  FOR(i, 32) x[i] = (u64) r[i];
  FOR(i, 32) FOR(j, 32) x[i + j] += h[i] * (u64) d[j];
  modL(sm + 32, x);

  FOR(i, 64) sig[i] = sm[i];

  free(sm);
  return 0;
}

/* Verify a 64-byte R||S signature over m (mlen bytes) with pk (32-byte
 * public key).  Returns 0 on success, -1 on failure. */
int
ed25519_verify(const u8 *sig, const u8 *m, size_t mlen, const u8 *pk)
{
  u8 *buf;
  u8 t[32], h[64];
  gf p[4], q[4];
  i64 i;
  int r = -1;

  buf = (u8 *) malloc(mlen + 64);
  if (!buf)
    return -1;

  FOR(i, 64) buf[i] = sig[i];
  FOR(i, (i64) mlen) buf[64 + i] = m[i];

  if (unpackneg(q, pk))
    goto out;

  FOR(i, 32) buf[i + 32] = pk[i];
  crypto_hash(h, buf, mlen + 64);
  reduce(h);
  scalarmult(p, q, h);

  scalarbase(q, sig + 32);
  add(p, q);
  pack(t, p);

  r = crypto_verify_32(sig, t);

out:
  free(buf);
  return r;
}

