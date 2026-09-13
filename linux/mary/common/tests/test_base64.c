#include <errno.h>

#include "common/base64.h"
#include "mary_test.h"

MARY_TEST(rfc_4648_vectors_encode) {
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const out[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    char buf[16];
    for (int i = 0; i < 7; i++) {
        size_t n = 0;
        MARY_ASSERT_EQ(mc_base64_encode((const unsigned char *)in[i], strlen(in[i]), buf, sizeof buf, &n), 0);
        MARY_ASSERT_STR(buf, out[i]);
        MARY_ASSERT_EQ(n, strlen(out[i]));
    }
    MARY_ASSERT_EQ(mc_base64_encode((const unsigned char *)"foo", 3, buf, 4, NULL), -ENOSPC);   /* no room for the NUL */
}

MARY_TEST(decoding_accepts_padding_or_none_and_nothing_else) {
    unsigned char buf[16];
    size_t n = 0;
    MARY_ASSERT_EQ(mc_base64_decode("Zm9vYmE=", 8, buf, sizeof buf, &n), 0);
    MARY_ASSERT_EQ(n, 5);
    MARY_ASSERT(memcmp(buf, "fooba", 5) == 0);
    MARY_ASSERT_EQ(mc_base64_decode("Zg", 2, buf, sizeof buf, &n), 0);
    MARY_ASSERT_EQ(n, 1);
    MARY_ASSERT_EQ(buf[0], 'f');
    MARY_ASSERT_EQ(mc_base64_decode("Zm9v!", 5, buf, sizeof buf, &n), -EINVAL);
    MARY_ASSERT_EQ(mc_base64_decode("Z", 1, buf, sizeof buf, &n), -EINVAL);
    MARY_ASSERT_EQ(mc_base64_decode("Zg=a", 4, buf, sizeof buf, &n), -EINVAL);
    MARY_ASSERT_EQ(mc_base64_decode("Zg=", 3, buf, sizeof buf, &n), -EINVAL);
    MARY_ASSERT_EQ(mc_base64_decode("Zm 9v", 5, buf, sizeof buf, &n), -EINVAL);
    MARY_ASSERT_EQ(mc_base64_decode("Zm9vYmFy", 8, buf, 5, &n), -ENOSPC);
}

MARY_TEST(pcm_samples_survive_a_round_trip) {
    /* 24 kHz float32 LE, as Voxtral TTS sends it: every byte value must come back. */
    unsigned char pcm[1024];
    for (int i = 0; i < 1024; i++) pcm[i] = (unsigned char)(i * 37 + 11);
    size_t elen = 0, dlen = 0;
    char *encoded = mc_base64_encode_alloc(pcm, sizeof pcm, &elen);
    MARY_ASSERT(encoded != NULL);
    MARY_ASSERT_EQ(elen, mc_base64_encoded_len(sizeof pcm));
    unsigned char *decoded = mc_base64_decode_alloc(encoded, elen, &dlen);
    MARY_ASSERT(decoded != NULL);
    MARY_ASSERT_EQ(dlen, sizeof pcm);
    MARY_ASSERT(memcmp(decoded, pcm, sizeof pcm) == 0);
    free(encoded);
    free(decoded);
    errno = 0;
    MARY_ASSERT(mc_base64_decode_alloc("@@@@", 4, &dlen) == NULL);
    MARY_ASSERT_EQ(errno, EINVAL);
}

int main(void) {
    MARY_RUN(rfc_4648_vectors_encode);
    MARY_RUN(decoding_accepts_padding_or_none_and_nothing_else);
    MARY_RUN(pcm_samples_survive_a_round_trip);
    MARY_TEST_MAIN_END();
}
