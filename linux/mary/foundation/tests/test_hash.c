#include <stdlib.h>

#include "foundation/hash.h"
#include "mary_test.h"

MARY_TEST(fnv1a_matches_the_swift_minters) {
    char hex[17];
    mf_fnv1a64_hex("", hex);
    MARY_ASSERT_STR(hex, "cbf29ce484222325");            /* the basis: an empty key */
    mf_fnv1a64_hex("a", hex);
    MARY_ASSERT_STR(hex, "af63dc4c8601ec8c");
    mf_fnv1a64_hex("foobar", hex);
    MARY_ASSERT_STR(hex, "85944171f73967e8");
}

MARY_TEST(djb2_wraps_like_a_swift_int) {
    MARY_ASSERT_EQ(mf_djb2(""), 5381);
    MARY_ASSERT_EQ(mf_djb2("a"), 5381 * 33 + 'a');
    /* Long input wraps past 2^63 and stays deterministic. */
    const char *s = "a very long span id that overflows sixty-four bits many times over, surely";
    int64_t once = mf_djb2(s);
    MARY_ASSERT_EQ(mf_djb2(s), once);
}

MARY_TEST(numeric_hash_renders_bytes_as_two_or_three_decimal_digits) {
    char out[MF_NUMERIC_HASH_MAX];
    /* SHA-256("") starts e3 b0 c4 42 98 fc 1c 14: 227 176 196 66 152 252 28 20 */
    mf_numeric_hash_text("", out);
    MARY_ASSERT_EQ(strncmp(out, "227176196661522522820", 21), 0);
    MARY_ASSERT(strlen(out) >= 64 && strlen(out) <= 96);
    /* SHA-256("abc") starts ba 78 16 bf: 186 120 22 191 */
    mf_numeric_hash("abc", 3, out);
    MARY_ASSERT_EQ(strncmp(out, "18612022191", 11), 0);
    /* An entity id the way Thread mints it: numericHash("<kind>|<name>"). */
    char a[MF_NUMERIC_HASH_MAX], b[MF_NUMERIC_HASH_MAX];
    mf_numeric_hash_text("person|ada lovelace", a);
    mf_numeric_hash_text("person|ada lovelace", b);
    MARY_ASSERT_STR(a, b);
    mf_numeric_hash_text("concept|ada lovelace", b);
    MARY_ASSERT(strcmp(a, b) != 0);
}

MARY_TEST(canonical_collapses_whitespace_and_lowercases) {
    char *c = mf_canonical("  Rao\t\n Studios\xC2\xA0 MaryOS  ");
    MARY_ASSERT_STR(c, "rao studios maryos");
    free(c);
    c = mf_canonical("Élan/Path-Name");
    MARY_ASSERT_STR(c, "\xC3\xA9lan/path-name");
    free(c);
    c = mf_canonical("");
    MARY_ASSERT_STR(c, "");
    free(c);
    /* The mary-unit id: fnv(canonical(owner) + "|" + unitKey). */
    char *owner = mf_canonical("Rao");
    char key[256], hex[17];
    snprintf(key, sizeof key, "%s|%s", owner, "maryos|linux/mary/README.md");
    mf_fnv1a64_hex(key, hex);
    MARY_ASSERT_EQ(strlen(hex), 16);
    free(owner);
}

int main(void) {
    MARY_RUN(fnv1a_matches_the_swift_minters);
    MARY_RUN(djb2_wraps_like_a_swift_int);
    MARY_RUN(numeric_hash_renders_bytes_as_two_or_three_decimal_digits);
    MARY_RUN(canonical_collapses_whitespace_and_lowercases);
    MARY_TEST_MAIN_END();
}
