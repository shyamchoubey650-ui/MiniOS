/* SHA-256 against the FIPS 180-4 test vectors and against hashes computed with an independent
 * implementation (Python's hashlib) for every padding boundary (55, 56, 63, 64, 65 ... bytes). */
#include <stdio.h>
#include <string.h>
#include "include/sha256.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void hex(const uint8_t *d, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static bool digest_is(const void *msg, size_t n, const char *want) { uint8_t d[32]; char h[65]; sha256(msg, n, d); hex(d, h); return !strcmp(h, want); }

int main(void) {
    puts("standard vectors");
    CHECK(digest_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(digest_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(digest_is(two, strlen(two), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    puts("padding boundaries (independent reference values)");
    static const struct { size_t n; const char *h; } v[] = {
        { 1,   "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb" },
        { 55,  "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
        { 56,  "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
        { 63,  "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
        { 64,  "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
        { 65,  "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
        { 119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
        { 120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
        { 128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
    };
    char a[200]; memset(a, 'a', sizeof a);
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) CHECK(digest_is(a, v[i].n, v[i].h));

    puts("streaming equals one-shot, for every chunk size");
    uint8_t whole[32];
    sha256(a, sizeof a, whole);
    for (size_t chunk = 1; chunk <= 130; chunk++) {
        sha256_ctx_t c; uint8_t d[32]; sha256_init(&c);
        for (size_t off = 0; off < sizeof a; off += chunk) sha256_update(&c, a + off, off + chunk > sizeof a ? sizeof a - off : chunk);
        sha256_final(&c, d);
        CHECK(memcmp(d, whole, 32) == 0);
    }
    puts("different input, different digest");
    uint8_t d1[32], d2[32];
    sha256("password1", 9, d1); sha256("password2", 9, d2);
    CHECK(memcmp(d1, d2, 32) != 0);
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
