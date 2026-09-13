#include "foundation/envelope.h"
#include "mary_test.h"

MARY_TEST(privacy_names_round_trip) {
    MARY_ASSERT_STR(mf_privacy_name(MF_PRIVACY_PUBLIC_DEFINITION), "publicDefinition");
    MARY_ASSERT_STR(mf_privacy_name(MF_PRIVACY_PRIVATE), "private");
    MARY_ASSERT_STR(mf_privacy_name(MF_PRIVACY_SENSITIVE), "sensitive");
    MARY_ASSERT_STR(mf_privacy_name(MF_PRIVACY_SECRET), "secret");
    MARY_ASSERT(mf_privacy_name((mf_privacy)9) == NULL);
    mf_privacy p = MF_PRIVACY_PUBLIC_DEFINITION;
    MARY_ASSERT(mf_privacy_from_name("secret", &p) && p == MF_PRIVACY_SECRET);
    MARY_ASSERT(!mf_privacy_from_name("open", &p));
    MARY_ASSERT(!mf_privacy_from_name(NULL, &p));
}

MARY_TEST(a_scope_resolves_as_far_as_it_is_proven) {
    mf_source_scope s = { 0 };
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_UNRESOLVED);
    s.device_id = "pi";
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_DEVICE);
    s.application_id = "textedit";
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_APPLICATION);
    s.window_id = "w3";
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_WINDOW);
    s.project_id = "maryos";
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_WORKSPACE);
    s.document_id = "";                 /* empty is absent, never inferred */
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_WORKSPACE);
    s.document_id = "/home/mary/Documents/note.txt";
    MARY_ASSERT_EQ(mf_source_scope_resolution(&s), MF_SOURCE_DOCUMENT);
    MARY_ASSERT_STR(mf_source_resolution_name(MF_SOURCE_DOCUMENT), "document");
    MARY_ASSERT_STR(mf_source_resolution_name(MF_SOURCE_UNRESOLVED), "unresolved");
}

int main(void) {
    MARY_RUN(privacy_names_round_trip);
    MARY_RUN(a_scope_resolves_as_far_as_it_is_proven);
    MARY_TEST_MAIN_END();
}
