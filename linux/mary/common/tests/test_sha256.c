#include <stdlib.h>

#include "common/sha256.h"
#include "mary_test.h"

static void hex_of(const void *data, size_t len, char out[65]) {
    unsigned char d[MC_SHA256_BYTES];
    mc_sha256(data, len, d);
    mc_sha256_hex(d, out);
}

MARY_TEST(the_fips_vectors_agree) {
    char hex[65];
    hex_of("", 0, hex);
    MARY_ASSERT_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hex_of("abc", 3, hex);
    MARY_ASSERT_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    hex_of(two, 56, hex);
    MARY_ASSERT_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

MARY_TEST(a_million_as_in_odd_pieces_hash_like_one_buffer) {
    char *a = malloc(1000000);
    memset(a, 'a', 1000000);
    mc_sha256_ctx c;
    mc_sha256_init(&c);
    size_t off = 0, step = 1;
    while (off < 1000000) {
        size_t n = step > 1000000 - off ? 1000000 - off : step;
        mc_sha256_update(&c, a + off, n);
        off += n;
        step = step * 3 + 7;
        if (step > 70000) step = 3;
    }
    unsigned char d[MC_SHA256_BYTES];
    char hex[65];
    mc_sha256_final(&c, d);
    mc_sha256_hex(d, hex);
    MARY_ASSERT_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(a);
}

int main(void) {
    MARY_RUN(the_fips_vectors_agree);
    MARY_RUN(a_million_as_in_odd_pieces_hash_like_one_buffer);
    MARY_TEST_MAIN_END();
}
