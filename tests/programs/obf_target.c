#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define FALLTHROUGH ((void)0)
#elif defined(__GNUC__) || defined(__clang__)
#define FALLTHROUGH __attribute__((fallthrough))
#else
#define FALLTHROUGH ((void)0)
#endif

#if defined(__clang__)
#define SHROUD(spec) __attribute__((annotate(spec)))
#else
#define SHROUD(spec)
#endif

static uint64_t g_digest = 0xcbf29ce484222325ULL;
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t g_counter = 0;
static jmp_buf g_jb;

static void feed(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ULL;
    }
}

static void feed_u64(uint64_t v) { feed(&v, sizeof v); }

static uint32_t rng32(void) {
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return (uint32_t)(x >> 32);
}

SHROUD("shroud;mba=2;virtualize=1")
static uint64_t bit_mixer(uint64_t x) {
    for (int i = 0; i < 6; i++) {
        x ^= x << 21;
        x ^= x >> 35;
        x ^= x << 4;
        x = (x << 17) | (x >> 47);
        x += 0x9E3779B97F4A7C15ULL;
        x &= (i & 1) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
    }
    uint32_t pc = 0;
    uint64_t t = x;
    while (t) {
        pc += (uint32_t)(t & 1u);
        t >>= 1;
    }
    return x ^ ((uint64_t)pc << 48) ^ ((x ^ (x >> 1) ^ (x >> 2) ^ (x >> 3)) & 0xFFu);
}

SHROUD("shroud;opaque=0;virtualize=1")
static uint32_t collatz_steps(uint32_t n) {
    uint32_t steps = 0;
    while (n != 1u) {
        n = (n & 1u) ? (3u * n + 1u) : (n >> 1);
        steps++;
    }
    return steps;
}

SHROUD("shroud;virtualize=1;mba=2")
static int tarai(int x, int y, int z) {
    if (x <= y) return y;
    return tarai(tarai(x - 1, y, z), tarai(y - 1, z, x), tarai(z - 1, x, y));
}

SHROUD("shroud;bogus=0;virtualize=1")
static int ack(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

SHROUD("shroud;virtualize=1")
static int32_t fib_rec(int n) { return n < 2 ? n : fib_rec(n - 1) + fib_rec(n - 2); }

SHROUD("shroud;virtualize=1")
static uint64_t fib_iter(int n) {
    uint64_t a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        uint64_t t = a + b;
        a = b;
        b = t;
    }
    return a;
}

SHROUD("shroud;virtualize=1;bpred=1")
static uint32_t gcd_iter(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

SHROUD("shroud;virtualize=1")
static uint32_t gcd_rec(uint32_t a, uint32_t b) { return b ? gcd_rec(b, a % b) : a; }

static int is_odd(int n);
SHROUD("shroud;virtualize=1")
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
SHROUD("shroud;virtualize=1")
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }

SHROUD("shroud;flatten=1;virtualize=1")
static int state_machine(const char *s) {
    int state = 0;
    int acc = 0;
    for (size_t i = 0; s[i]; i++) {
        int c = (unsigned char)s[i];
        switch (state) {
        case 0:
            if (c == 'a') state = 1;
            else acc += c & 7;
            break;
        case 1:
            if (c == 'b') state = 2;
            else if (c == 'a') state = 1;
            else { state = 0; acc ^= c; }
            break;
        case 2:
            if (c == 'c') { acc += 13; state = 3; }
            break;
        default:
            if (c == '#') state = 0;
            acc = (acc * 3 + c) & 0xFFFF;
            break;
        }
    }
    return acc ^ (state * 0x9E37);
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

SHROUD("shroud;mba=3;virtualize=1")
static uint32_t key_hash(const char *key, uint32_t salt) {
    uint32_t h = 0x811C9DC5u ^ salt;
    for (size_t i = 0; key[i]; i++) {
        h ^= (unsigned char)key[i];
        h *= 0x01000193u;
        h = (h << 13) | (h >> 19);
    }
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}

SHROUD("shroud;virtualize=1")
static int validate_key(const char *key) {
    if (strlen(key) != 16) return 0;
    return (key_hash(key, 0x5A17u) & 0xFFFFu) == 0x4E21u;
}

struct packet {
    uint8_t version;
    uint8_t flags;
    uint16_t length;
    uint32_t seq;
};

SHROUD("shroud;virtualize=1")
static uint32_t packet_parse(const uint8_t *buf) {
    struct packet p;
    memcpy(&p, buf, sizeof p);
    p.length = (uint16_t)((p.length >> 8) | (p.length << 8));
    p.seq = ((p.seq & 0xFF000000u) >> 24) | ((p.seq & 0x00FF0000u) >> 8) |
            ((p.seq & 0x0000FF00u) << 8) | ((p.seq & 0x000000FFu) << 24);
    uint32_t r = ((uint32_t)p.version << 24) | ((uint32_t)p.flags << 16) | p.length;
    return r ^ p.seq;
}

struct bits {
    unsigned a : 3;
    unsigned b : 5;
    unsigned c : 7;
    unsigned d : 17;
};

SHROUD("shroud;virtualize=1")
static uint32_t bitfield_fold(const struct bits *v) {
    return (v->a * 31u) ^ (v->b * 17u) ^ (v->c * 7u) ^ (v->d * 3u);
}

typedef int32_t (*binop)(int32_t, int32_t);

static int32_t op_add(int32_t a, int32_t b) { return a + b; }
static int32_t op_sub(int32_t a, int32_t b) { return a - b; }
static int32_t op_xor(int32_t a, int32_t b) { return a ^ b; }
static int32_t op_mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t op_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t op_max(int32_t a, int32_t b) { return a > b ? a : b; }

SHROUD("shroud;virtualize=1")
static int32_t opvm_run(const uint8_t *prog, size_t n, int32_t seed) {
    binop table[6] = {op_add, op_sub, op_xor, op_mul, op_min, op_max};
    int32_t acc = seed;
    for (size_t i = 0; i < n; i++) {
        uint8_t op = (uint8_t)(prog[i] & 7u);
        int32_t arg = (int32_t)(prog[i] >> 3) - 16;
        if (op < 6) acc = table[op](acc, arg);
        else acc = (int32_t)((uint32_t)acc << (prog[i] & 3u)) ^ arg;
    }
    return acc;
}

SHROUD("shroud;virtualize=1")
static uint32_t matmul(void) {
    enum { N = 12 };
    int32_t *a = (int32_t *)malloc(sizeof(int32_t) * (size_t)N * N);
    int32_t *b = (int32_t *)malloc(sizeof(int32_t) * (size_t)N * N);
    int32_t *c = (int32_t *)calloc((size_t)N * N, sizeof(int32_t));
    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return 0;
    }
    for (int i = 0; i < N * N; i++) {
        a[i] = (int32_t)(rng32() & 0xFFu) - 128;
        b[i] = (int32_t)(rng32() & 0xFFu) - 128;
    }
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++)
            for (int j = 0; j < N; j++)
                c[i * N + j] = (int32_t)((uint32_t)c[i * N + j] +
                                         (uint32_t)a[i * N + k] * (uint32_t)b[k * N + j]);
    uint32_t h = 2166136261u;
    for (int i = 0; i < N * N; i++) h = (h ^ (uint32_t)c[i]) * 16777619u;
    free(a);
    free(b);
    free(c);
    return h;
}

SHROUD("shroud;bogus=0;virtualize=1")
static void qsort_i32(int32_t *a, int lo, int hi) {
    if (lo >= hi) return;
    int32_t p = a[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
        while (a[i] < p) i++;
        while (a[j] > p) j--;
        if (i <= j) {
            int32_t t = a[i];
            a[i] = a[j];
            a[j] = t;
            i++;
            j--;
        }
    }
    qsort_i32(a, lo, j);
    qsort_i32(a, i, hi);
}

static uint32_t qsort_test(void) {
    int32_t arr[64];
    for (int i = 0; i < 64; i++) arr[i] = (int32_t)(rng32() % 1000u) - 500;
    qsort_i32(arr, 0, 63);
    uint32_t h = 5381u;
    for (int i = 0; i < 64; i++) h = h * 33u + ((uint32_t)arr[i] & 0xFFu);
    return h;
}

SHROUD("shroud")
static uint64_t vararg_mix(int count, ...) {
    va_list ap;
    va_start(ap, count);
    uint64_t h = 0;
    for (int i = 0; i < count; i++) {
        uint64_t v = va_arg(ap, uint64_t);
        h = h * 0x100000001b3ULL ^ v;
        h = (h << 7) | (h >> 57);
    }
    va_end(ap);
    return h;
}

static int safe_div(int a, int b) {
    if (b == 0) longjmp(g_jb, 1);
    return a / b;
}

SHROUD("shroud;opaque=0")
static uint32_t setjmp_test(void) {
    volatile uint32_t acc = 0;
    static const int vals[6][2] = {
        {100, 7}, {55, 0}, {42, 3}, {9, 0}, {88, 11}, {7, 2}};
    for (int i = 0; i < 6; i++) {
        if (setjmp(g_jb) == 0)
            acc = acc * 33u + (uint32_t)safe_div(vals[i][0], vals[i][1]);
        else
            acc = acc * 33u + 0xBADD;
    }
    return (uint32_t)acc;
}

static uint64_t fp_eval(void) {
    static const double coef[6] = {1.5, -2.25, 3.125, -0.5, 0.0625, 7.0};
    double x = 0.75, y = 0.0;
    for (int i = 5; i >= 0; i--) y = y * x + coef[i];
    double s = sqrt(fabs(y));
    uint64_t r, r2;
    memcpy(&r, &y, sizeof r);
    memcpy(&r2, &s, sizeof r2);
    return r ^ (r2 * 3u);
}

SHROUD("shroud;virtualize=1")
static uint32_t memmove_test(void) {
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i * 7 + 3);
    memmove(buf + 8, buf, 40);
    memmove(buf, buf + 16, 32);
    uint32_t h = 0;
    for (int i = 0; i < 64; i++) h = h * 131u + buf[i];
    return h;
}

SHROUD("shroud;mba=2")
static uint64_t signed_unsigned_mix(int32_t a, uint32_t b, int16_t c, uint16_t d) {
    uint64_t r = (uint64_t)(uint32_t)a;
    r = r * 0x9E3779B1u + b;
    r ^= (uint64_t)(uint16_t)c << 32;
    r += (uint64_t)d * 0x85EBCA6Bu;
    r ^= r >> 29;
    int32_t s = (int32_t)(r & 0x7FFFFFFFu);
    r += (uint64_t)(uint32_t)(s / 7);
    r ^= (uint64_t)(uint32_t)(s % 7);
    return r;
}

SHROUD("shroud;opaque=1;bogus=1;bpred=1;virtualize=1")
static uint64_t predicate_walk(uint32_t x) {
    uint64_t acc = x;
    if (((x * (x + 1u)) & 1u) == 0) acc += 0x1234;
    if ((((x * x) - x) & 1u) == 0) acc ^= 0x5555;
    if ((x | (x ^ 0xFFFFFFFFu)) == 0xFFFFFFFFu) acc *= 3;
    if (((x << 1) >> 1) == (x & 0x7FFFFFFFu)) acc ^= 0xAAAA;
    if (((x + 1u) > x) || (x == 0xFFFFFFFFu)) acc += 99;
    switch (x & 3u) {
    case 0: acc += x; break;
    case 1: acc ^= x >> 1; break;
    case 2: acc -= x >> 2; break;
    case 3: acc |= 0xF0F0F0F0ULL; break;
    }
    return acc;
}

SHROUD("shroud;virtualize=1;flatten=1")
static uint32_t loop_zoo(uint32_t n) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            if (((i ^ j) & 3u) == 0) continue;
            if ((i * j) > 400u) break;
            acc += (i * 31u) ^ (j * 17u);
        }
        if (acc & 1u) acc ^= 0x5A5A5A5Au;
        if (i == 13u) goto done;
    }
done:
    return acc;
}

SHROUD("shroud;virtualize=1;flatten=1")
static uint32_t fallthrough_switch(uint32_t x) {
    uint32_t acc = 0;
    switch (x & 7u) {
    case 0: acc += 1; FALLTHROUGH;
    case 1: acc += 2; FALLTHROUGH;
    case 2: acc += 3; break;
    case 3: acc += 4; FALLTHROUGH;
    case 4: acc += 5; break;
    case 5: acc += 6; FALLTHROUGH;
    case 6: acc += 7; FALLTHROUGH;
    case 7: acc += 8; break;
    }
    return acc;
}

SHROUD("shroud;mba=1;virtualize=1")
static uint64_t deep_args(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                          uint64_t e, uint64_t f, uint64_t g, uint64_t h,
                          uint64_t i, uint64_t j, uint64_t k, uint64_t l) {
    uint64_t r = a ^ (b + c) ^ (d - e) ^ (f * g) ^ (h & i) ^ (j | k) ^ l;
    r = (r << 7) | (r >> 57);
    r += (a * b) ^ (c * d);
    r ^= (e << 3) + (f >> 2);
    r = r * 0x9E3779B97F4A7C15ULL + (g ^ h) + (i & j) + (k | l);
    return r;
}

SHROUD("shroud;virtualize=1")
static uint32_t bump(uint32_t x) {
    g_counter += x * 3u + 1u;
    return g_counter ^ (g_counter >> 3);
}

static const uint8_t g_blob[128] = {
    0x4A, 0xC3, 0x91, 0x7E, 0x2D, 0xF8, 0x60, 0x15,
    0xBB, 0x06, 0xD2, 0x39, 0xEC, 0x55, 0x88, 0x21,
    0x73, 0xAE, 0x4F, 0xC0, 0x1B, 0x96, 0xE4, 0x3A,
    0x57, 0x89, 0xBC, 0x02, 0xDF, 0x6C, 0x31, 0xA5,
    0xE8, 0x47, 0x9A, 0x24, 0x6F, 0xD1, 0x08, 0xB3,
    0x5C, 0xC7, 0x32, 0x8D, 0xF0, 0x19, 0xA4, 0x6B,
    0x2E, 0xD5, 0x78, 0x03, 0xBE, 0x41, 0x97, 0x6A,
    0x0C, 0xE3, 0x54, 0xA9, 0x7F, 0x28, 0xCB, 0x16,
    0x95, 0x30, 0xFA, 0x6D, 0x82, 0x1E, 0xB7, 0x40,
    0x69, 0xD4, 0x0B, 0xAD, 0x36, 0xF1, 0x5E, 0x83,
    0x27, 0x8A, 0xCD, 0x10, 0xE6, 0x59, 0xB2, 0x74,
    0x1F, 0xF4, 0x63, 0x98, 0x05, 0xDA, 0x2B, 0x80,
    0xAB, 0x3F, 0xD6, 0x11, 0x8C, 0x57, 0xE0, 0x22,
    0x7B, 0xC4, 0x0E, 0xA1, 0x35, 0xF7, 0x68, 0x9D,
    0x14, 0xD9, 0x42, 0xB6, 0x0A, 0xED, 0x61, 0x38,
    0x86, 0x2A, 0xCF, 0x50, 0x9F, 0x13, 0xE9, 0x44
};

SHROUD("shroud;virtualize=1;consts=1")
static uint32_t blob_sum(void) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < sizeof g_blob; i++) {
        a = (a << 5) + a + g_blob[i];
        b = (b >> 3) ^ ((uint32_t)g_blob[i] << (i & 7));
    }
    return a ^ b;
}

SHROUD("shroud;strings=1;virtualize=0;opaque=1")
int main(void) {
    uint64_t r;

    r = bit_mixer(0x0123456789ABCDEFULL);
    printf("[01] bit_mixer          = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = collatz_steps(0xDEADBEEFu);
    printf("[02] collatz_steps      = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = (uint64_t)(uint32_t)tarai(12, 6, 0);
    printf("[03] tarai              = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = (uint64_t)(uint32_t)ack(3, 4);
    printf("[04] ackermann          = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = fib_iter(90) ^ (uint64_t)(uint32_t)fib_rec(24);
    printf("[05] fib                = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = (uint64_t)gcd_iter(46209822u, 1288704u) + (uint64_t)gcd_rec(0x8E1DA5u, 0x3C6EF35Fu);
    printf("[06] gcd                = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = (uint64_t)(uint32_t)state_machine("aaabc#abcaabxyz#abc");
    printf("[07] state_machine      = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = fnv1a("virtual machine") ^ fnv1a("opaque predicates are math, not magic");
    printf("[08] fnv strings        = %016" PRIx64 "\n", r);
    feed_u64(r);

    {
        static const uint8_t pkt[8] = {0x02, 0x17, 0x01, 0x2A, 0xDE, 0xAD, 0xBE, 0xEF};
        r = packet_parse(pkt);
        printf("[09] packet_parse       = %016" PRIx64 "\n", r);
        feed_u64(r);
    }

    {
        struct bits b = {6, 21, 99, 100000};
        r = bitfield_fold(&b);
        printf("[10] bitfield_fold      = %016" PRIx64 "\n", r);
        feed_u64(r);
    }

    {
        static const uint8_t prog[32] = {
            0x00, 0x0B, 0x1A, 0x23, 0x38, 0x4D, 0x56, 0x6F,
            0x74, 0x81, 0x9E, 0xA3, 0xB0, 0xC5, 0xD2, 0xEF,
            0x11, 0x2C, 0x37, 0x42, 0x5D, 0x68, 0x73, 0x8E,
            0x99, 0xA4, 0xBF, 0xC0, 0xDB, 0xE6, 0xF1, 0x0A};
        r = (uint64_t)(uint32_t)opvm_run(prog, sizeof prog, 0x1234);
        printf("[11] opvm_run           = %016" PRIx64 "\n", r);
        feed_u64(r);
    }

    r = matmul();
    printf("[12] matmul             = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = qsort_test();
    printf("[13] qsort              = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = vararg_mix(5, (uint64_t)0x11, (uint64_t)0x2233, (uint64_t)0x445566,
                   (uint64_t)0x778899AA, (uint64_t)0xBBCCDDEEFFULL);
    printf("[14] vararg_mix         = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = setjmp_test();
    printf("[15] setjmp             = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = fp_eval();
    printf("[16] fp_eval            = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = memmove_test();
    printf("[17] memmove            = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = signed_unsigned_mix(-123456789, 0xDEADBEEFu, -12345, 54321);
    printf("[18] signed_unsigned    = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = predicate_walk(0xA5A5F00Du);
    printf("[19] predicate_walk     = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = loop_zoo(20);
    printf("[20] loop_zoo           = %016" PRIx64 "\n", r);
    feed_u64(r);

    {
        uint32_t s = 0;
        for (uint32_t i = 0; i < 8; i++) s = s * 31u + fallthrough_switch(i);
        r = s;
        printf("[21] fallthrough_switch = %016" PRIx64 "\n", r);
        feed_u64(r);
    }

    r = deep_args(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    printf("[22] deep_args          = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = 0;
    for (uint32_t i = 1; i <= 10; i++) r ^= bump(i);
    printf("[23] globals            = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = (uint64_t)(uint32_t)is_even(41) | ((uint64_t)(uint32_t)is_odd(41) << 1);
    printf("[24] mutual_recursion   = %016" PRIx64 "\n", r);
    feed_u64(r);

    r = blob_sum();
    printf("[25] blob_sum           = %016" PRIx64 "\n", r);
    feed_u64(r);

    {
        static const char *keys[3] = {"ABCDEFGHIJKLMNOP", "0123456789abcdef", "Sup3rS3cr3tK3y!!"};
        r = 0;
        for (int i = 0; i < 3; i++) {
            r = r * 31u + key_hash(keys[i], 0x5A17u);
            r = r * 31u + (uint64_t)(uint32_t)validate_key(keys[i]);
        }
        printf("[26] validate_key       = %016" PRIx64 "\n", r);
        feed_u64(r);
    }

    r = 0;
    for (int i = 0; i < 16; i++) r ^= (uint64_t)rng32() << (i & 3);
    printf("[27] rng stream         = %016" PRIx64 "\n", r);
    feed_u64(r);

    printf("DIGEST = %016" PRIx64 "\n", g_digest);
    return 0;
}
