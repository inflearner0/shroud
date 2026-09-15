#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__clang__)
#define SHROUD(spec) __attribute__((annotate(spec)))
#else
#define SHROUD(spec)
#endif

#define KEY_LEN 16
#define SALT 0x5A17u

#define K_SUM 1373u
#define K_HASH 0xCEB721C7u
#define K_FNVA 8647u
#define K_XOR 55u
#define K_ADD 202u
#define K_MAGIC 0x5EED1234u

SHROUD("shroud;virtualize=1;mba=3")
static uint32_t stage_hash(const char *s, uint32_t salt) {
    uint32_t h = 0x811C9DC5u ^ salt;
    for (size_t i = 0; s[i]; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x01000193u;
        h = (h << 13) | (h >> 19);
    }
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}

SHROUD("shroud;virtualize=1;mba=2")
static int stage_relations(const char *k) {
    uint32_t sum = 0;
    for (int i = 0; i < KEY_LEN; i++) sum += (unsigned char)k[i];
    if (sum != K_SUM) return 0;
    if (((unsigned char)k[0] ^ (unsigned char)k[5]) != K_XOR) return 0;
    if (((unsigned char)k[3] + 2u * (unsigned char)k[7]) != K_ADD) return 0;
    return 1;
}

SHROUD("shroud;virtualize=1;mba=1;flatten=1")
static int key_check(const char *k) {
    if (strlen(k) != KEY_LEN) return 0;
    if (!stage_relations(k)) return 0;
    uint32_t h = stage_hash(k, SALT);
    if ((h & 0xFFFFu) != K_FNVA) return 0;
    if (h != K_HASH) return 0;
    return 1;
}

SHROUD("shroud;virtualize=1")
static int unlock_alt(const char *k) {
    uint32_t h = stage_hash(k, 0xDEADu);
    if (h == 0x11223344u) return 1;
    if (strlen(k) == KEY_LEN && k[KEY_LEN - 1] == '!') return 1;
    return 0;
}

SHROUD("shroud;strings=1")
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <key>\n", argv[0]);
        return 2;
    }
    (void)unlock_alt(argv[1]);
    if (key_check(argv[1])) {
        printf("Access granted\n");
        printf("FLAG{shroud:%08x}\n", stage_hash(argv[1], SALT) ^ K_MAGIC);
        return 0;
    }
    printf("Access denied\n");
    return 1;
}
