
#ifdef __SIZEOF_INT128__

typedef __uint128_t uint128_t;

#define U128_0                          ((uint128_t) 0)
#define u128zero(a)                     a = (uint128_t) 0
#define u128seti(a,b)                   a = (b)
#define u128add(a,b)                    a += (b)
#define u128addi(a,b)                   a += ((uint64_t) (b))
#define u128subi(a,b)                   a -= ((uint64_t) (b))
#define u128mul32(a,b)                  a *= ((uint64_t) (b))
#define u128sub(a,b)                    a -= (b)
#define u128mul(a,b,c)                  a = widemul(b, c)
#define u128muladd(a,b,c)               a += widemul(b, c)
#define u128mulsub(a,b,c)               a -= widemul(b, c)
#define u128shft(a,b)                   a >>= (b)
#define u128lo(a)                       ((uint64_t) (a))
#define _u128shft(a,b)                  ((a) >> (b))
#define s128shft(a,b)                   (a) = (uint64_t) (((__int64_t) (a)) >> (b))
#define u128loiszero(a)                 is_zero(a)

static inline uint128_t widemul (const uint64_t a, const uint64_t b)
{
    return ((uint128_t) a) * ((uint128_t) b);
}

static inline uint64_t is_zero (uint64_t x)
{
    return (uint64_t) ((((uint128_t) (x)) - 1) >> 64);
}

#else


struct struct128bit {
    uint64_t hi;
    uint64_t lo;
} __attribute__ ((aligned (32)));

typedef struct struct128bit uint128_t;

#define U128_0                          {(uint64_t) 0, (uint64_t) 0}
#define u128zero(a)                     a.lo = a.hi = (uint64_t) 0
#define u128seti(a,b)                   do { a.lo = (b); a.hi = (uint64_t) 0; } while (0);
#define u128add(a,b)                    wideadd (&(a), &(b))
#define u128addi(a,b)                   wideadd64 (&(a), (b))
#define u128subi(a,b)                   widesub64 (&(a), (b))
#define u128mul32(a,b)                  widemul32 (&(a), (b))
#define u128sub(a,b)                    widesub (&(a), &(b))
#define u128mul(a,b,c)                  widemul(&(a), (b), (c))
#define u128muladd(a,b,c)          do { uint128_t __t; \
                                        widemul(&__t, (b), (c)); \
                                        wideadd (&(a), &__t); } while (0)
#define u128mulsub(a,b,c)          do { uint128_t __t; \
                                        widemul(&__t, (b), (c)); \
                                        widesub (&(a), &__t); } while (0)
#define u128shft(a,b)                   wideshft(&(a), b)
#define u128lo(a)                       (a).lo
#define _u128shft(a,b)                  wideshft64(&(a), b)
#define s128shft(a,b)                   swideshft(&(a), b)
#define u128loiszero(a)                 is_zero((a).lo)

static inline uint64_t wideshft64 (uint128_t *a, int b)
{
    return (a->lo >> b) | (a->hi << (64 - b));
}

static inline void wideshft (uint128_t *a, int b)
{
    a->lo = (a->lo >> b) | (a->hi << (64 - b));
    a->hi = (a->hi >> b);
}

/* not used for shifts more than 63 */
static inline void swideshft (uint128_t *a, int b)
{
    a->lo = (a->lo >> b) | (a->hi << (64 - b));
    a->hi = (uint64_t) (((int64_t) a->hi) >> b);
}

static inline void wideadd (uint128_t *a, uint128_t *b)
{
    uint64_t tmp;
    tmp = a->lo;
    a->lo += b->lo;
    if (a->lo < tmp)
        a->hi++;                /* carry */
    a->hi += b->hi;
}

static inline void wideadd64 (uint128_t *a, uint64_t b)
{
    uint64_t tmp;
    tmp = a->lo;
    a->lo += b;
    if (a->lo < tmp)
        a->hi++;                /* carry */
}

static inline void widesub (uint128_t *a, uint128_t *b)
{
    uint64_t tmp;
    tmp = a->lo;
    a->lo -= b->lo;
    if (a->lo > tmp)
        a->hi--;                /* carry */
    a->hi -= b->hi;
}

static inline void widesub64 (uint128_t *a, uint64_t b)
{
    uint64_t tmp;
    tmp = a->lo;
    a->lo -= b;
    if (a->lo > tmp)
        a->hi--;                /* carry */
}

/* a *= b */
static inline void widemul32 (uint128_t *a, const uint32_t b)
{
    uint64_t t0, t1, t2, t3;
    uint32_t c0, c1, c2, c3, r, carry = 0;

    t0 = (a->lo & 0xffffffffUL) * b;
    t1 = (a->lo >> 32) * b;

    t2 = (a->hi & 0xffffffffUL) * b;
    t3 = (a->hi >> 32) * b;

    c0 = t0 & 0xffffffffUL;
    c1 = t1 & 0xffffffffUL;
    c2 = t2 & 0xffffffffUL;
    c3 = t3 & 0xffffffffUL;

    r = c1; c1 += (t0 >> 32)        ; carry = (c1 < r);
    r = c2; c2 += (t1 >> 32) + carry; carry = (c2 < r);
    r = c3; c3 += (t2 >> 32) + carry; carry = (c3 < r);

    a->lo = c0 | ((uint64_t) c1 << 32);
    a->hi = c2 | ((uint64_t) c3 << 32);
}

/* c = a * b */
static inline void widemul (uint128_t *c, const uint64_t a, const uint64_t b)
{
    uint64_t a_hi, a_lo;
    uint64_t b_hi, b_lo;

    uint64_t lo;
    uint64_t inner1;
    uint64_t inner2;
    uint64_t hi;

    uint64_t tmp;

    a_hi = a >> 32;
    a_lo = a & 0xffffffffUL;

    b_hi = b >> 32;
    b_lo = b & 0xffffffffUL;

    lo = a_lo * b_lo;
    inner1 = a_hi * b_lo;
    inner2 = a_lo * b_hi;
    hi = a_hi * b_hi;

    tmp = lo;
    lo += (inner1 << 32);
    if (lo < tmp)
        hi++;                   /* carry */

    tmp = lo;
    lo += (inner2 << 32);
    if (lo < tmp)
        hi++;                   /* carry */

    hi += (inner1 >> 32);
    hi += (inner2 >> 32);

    c->lo = lo;
    c->hi = hi;
}

static inline uint64_t is_zero (uint64_t a)
{
    a = (((a >> 32) | (a & 0xffffffffUL)) - 1) >> 32;
    return a | (a << 32);
}

#endif

