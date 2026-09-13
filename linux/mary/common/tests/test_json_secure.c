#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "common/secure.h"
#include "mary_test.h"

MARY_TEST(parsing_is_strict_about_what_surrounds_the_value) {
    const char *ok = "  {\"type\":\"key.status\",\"present\":true,\"verified_at\":1757700000000}\n";
    struct json_object *obj = mc_json_parse(ok, strlen(ok));
    MARY_ASSERT(obj != NULL);
    MARY_ASSERT_STR(mc_json_type(obj), "key.status");
    bool present = false;
    int64_t at = 0;
    MARY_ASSERT(mc_json_bool(obj, "present", &present) && present);
    MARY_ASSERT(mc_json_int64(obj, "verified_at", &at));
    MARY_ASSERT_EQ(at, 1757700000000LL);
    json_object_put(obj);

    MARY_ASSERT(mc_json_parse("{\"a\":1} x", 9) == NULL);
    MARY_ASSERT(mc_json_parse("{\"a\":", 5) == NULL);
    struct json_object *number = mc_json_parse("12", 2);   /* a top-level number needs no terminator */
    MARY_ASSERT(number != NULL && json_object_get_int(number) == 12);
    json_object_put(number);
}

MARY_TEST(getters_never_coerce_a_wrong_type) {
    const char *text = "{\"rms\":0.25,\"n\":3,\"flag\":\"true\",\"inner\":{\"x\":1},\"list\":[1]}";
    struct json_object *obj = mc_json_parse(text, strlen(text));
    double d = 0;
    MARY_ASSERT(mc_json_double(obj, "rms", &d));
    MARY_ASSERT_NEAR(d, 0.25, 1e-9);
    MARY_ASSERT(mc_json_double(obj, "n", &d));      /* an integer reads as a double */
    MARY_ASSERT_NEAR(d, 3.0, 1e-9);
    MARY_ASSERT(!mc_json_int64(obj, "rms", NULL));
    MARY_ASSERT(!mc_json_bool(obj, "flag", NULL));
    MARY_ASSERT(mc_json_string(obj, "n") == NULL);
    MARY_ASSERT(mc_json_string(obj, "missing") == NULL);
    MARY_ASSERT(mc_json_object(obj, "inner") != NULL);
    MARY_ASSERT(mc_json_object(obj, "list") == NULL);
    MARY_ASSERT(mc_json_array(obj, "list") != NULL);
    MARY_ASSERT(mc_json_type(obj) == NULL);
    size_t len = 0;
    struct json_object *path = json_object_new_object();
    json_object_object_add(path, "dir", json_object_new_string("/var/lib/thread"));
    MARY_ASSERT_STR(mc_json_compact(path, &len), "{\"dir\":\"/var/lib/thread\"}");
    json_object_put(path);
    json_object_put(obj);
}

MARY_TEST(secrets_are_wiped_and_compared_in_constant_time) {
    char key[33] = "sk-0123456789abcdef0123456789abc";
    MARY_ASSERT(mc_secure_equal(key, "sk-0123456789abcdef0123456789abc", 32));
    MARY_ASSERT(!mc_secure_equal(key, "sk-0123456789abcdef0123456789abd", 32));
    mc_secure_zero(key, sizeof key);
    for (size_t i = 0; i < sizeof key; i++) MARY_ASSERT_EQ(key[i], 0);
    mc_secure_zero(NULL, 4);
}

MARY_TEST(clocks_move_forward) {
    int64_t a = mc_now_ms(), b = mc_now_ms();
    MARY_ASSERT(b >= a);
    MARY_ASSERT(mc_wall_ms() > 1700000000000LL);
    mc_log_init("test");
    mc_log_set_threshold(MC_LOG_ERROR);
    mc_log(MC_LOG_INFO, "not printed: below the threshold");
}

int main(void) {
    MARY_RUN(parsing_is_strict_about_what_surrounds_the_value);
    MARY_RUN(getters_never_coerce_a_wrong_type);
    MARY_RUN(secrets_are_wiped_and_compared_in_constant_time);
    MARY_RUN(clocks_move_forward);
    MARY_TEST_MAIN_END();
}
