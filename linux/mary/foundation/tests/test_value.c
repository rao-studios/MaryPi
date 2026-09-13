#include <math.h>

#include "foundation/envelope.h"
#include "foundation/value.h"
#include "mary_test.h"

MARY_TEST(objects_compare_as_dictionaries) {
    mf_value *a = mf_value_object(), *b = mf_value_object();
    mf_value_object_set(a, "role", mf_value_string("user"));
    mf_value_object_set(a, "turn", mf_value_integer(3));
    mf_value_object_set(b, "turn", mf_value_integer(3));
    mf_value_object_set(b, "role", mf_value_string("user"));
    MARY_ASSERT(mf_value_equal(a, b));
    mf_value_object_set(b, "role", mf_value_string("assistant"));   /* replaces, like a dictionary */
    MARY_ASSERT_EQ(b->as.object.count, 2);
    MARY_ASSERT(!mf_value_equal(a, b));
    mf_value_free(a);
    mf_value_free(b);
}

MARY_TEST(arrays_are_ordered_and_kinds_never_mix) {
    mf_value *a = mf_value_array(), *b = mf_value_array();
    mf_value_array_append(a, mf_value_number(1.5));
    mf_value_array_append(a, mf_value_null());
    mf_value_array_append(b, mf_value_null());
    mf_value_array_append(b, mf_value_number(1.5));
    MARY_ASSERT(!mf_value_equal(a, b));
    mf_value *one = mf_value_integer(1), *one_point_zero = mf_value_number(1.0);
    MARY_ASSERT(!mf_value_equal(one, one_point_zero));     /* integer and number are different cases */
    const unsigned char bytes[] = { 0, 1, 2 };
    mf_value *d1 = mf_value_data(bytes, 3), *d2 = mf_value_data(bytes, 3);
    MARY_ASSERT(mf_value_equal(d1, d2));
    mf_value_free(a); mf_value_free(b); mf_value_free(one); mf_value_free(one_point_zero); mf_value_free(d1); mf_value_free(d2);
}

MARY_TEST(an_envelope_declares_the_swift_shape) {
    mf_envelope e = { .type_id = "text.passage", .schema_version = "1.0.0", .value = mf_value_string("hello"),
                      .scope = { .application_id = "textedit", .window_id = "w1" },
                      .privacy = MF_PRIVACY_PRIVATE, .created_at = 0, .expires_at = NAN };
    MARY_ASSERT_STR(mf_privacy_name(e.privacy), "private");
    MARY_ASSERT_STR(mf_privacy_name(MF_PRIVACY_PUBLIC_DEFINITION), "publicDefinition");
    MARY_ASSERT(mf_privacy_name((mf_privacy)9) == NULL);
    MARY_ASSERT(isnan(e.expires_at));
    mf_value_free(e.value);
}

int main(void) {
    MARY_RUN(objects_compare_as_dictionaries);
    MARY_RUN(arrays_are_ordered_and_kinds_never_mix);
    MARY_RUN(an_envelope_declares_the_swift_shape);
    MARY_TEST_MAIN_END();
}
