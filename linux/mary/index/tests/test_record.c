#include "test_support.h"

#include "common/json.h"
#include "index/record.h"

MARY_TEST(a_text_file_is_chunked_and_a_picture_is_one_line_and_the_hash_is_stable) {
    make_home();
    put_file("Documents/work/notes.txt", "Paris is the capital of France.\n\nBread is good.\n");
    put_file("Pictures/cat.png", "\x89PNG\r\n");
    char path[512];
    snprintf(path, sizeof path, "%s/Documents/work/notes.txt", home);
    int error = 0;
    struct json_object *r = ix_record(home, "rao", path, &error);
    MARY_ASSERT(r != NULL);
    MARY_ASSERT_STR(mc_json_type(r), "deposit");
    MARY_ASSERT_STR(mc_json_string(r, "group"), "files-rao");
    MARY_ASSERT_STR(mc_json_string(r, "label"), "Files");
    MARY_ASSERT_STR(mc_json_string(r, "family"), "file");
    MARY_ASSERT_STR(mc_json_string(r, "name"), "notes.txt");
    MARY_ASSERT_STR(mc_json_string(r, "text"), "Paris is the capital of France.\n\nBread is good.\n");
    bool chunk = false;
    MARY_ASSERT(mc_json_bool(r, "chunk", &chunk) && chunk);
    struct json_object *file = mc_json_object(r, "file");
    MARY_ASSERT_STR(mc_json_string(file, "path"), path);
    MARY_ASSERT_STR(mc_json_string(file, "kind"), "text");
    int64_t size = 0;
    MARY_ASSERT(mc_json_int64(file, "size", &size) && size == 48);
    bool indexed = false;
    MARY_ASSERT(mc_json_bool(file, "text_indexed", &indexed) && indexed);
    char hash[65];
    MARY_ASSERT_EQ(ix_hash_file(path, hash), 0);
    MARY_ASSERT_STR(mc_json_string(file, "hash"), hash);
    MARY_ASSERT_EQ(strlen(hash), 64);
    MARY_ASSERT_STR(mc_json_string(mc_json_object(r, "metadata"), "kind"), "text");
    /* the graph: the file, work, Documents, ~ */
    struct json_object *entities = mc_json_array(r, "entities"), *relationships = mc_json_array(r, "relationships");
    MARY_ASSERT_EQ(json_object_array_length(entities), 4);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(entities, 0), "name"), "Documents/work/notes.txt");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(entities, 0), "kind"), "file");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(entities, 1), "name"), "Documents/work");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(entities, 1), "kind"), "folder");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(entities, 3), "name"), "~");
    MARY_ASSERT_EQ(json_object_array_length(relationships), 3);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(relationships, 0), "subject"), "Documents/work/notes.txt");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(relationships, 0), "predicate"), "in");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(relationships, 0), "object"), "Documents/work");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(relationships, 2), "object"), "~");
    json_object_put(r);

    snprintf(path, sizeof path, "%s/Pictures/cat.png", home);
    r = ix_record(home, "rao", path, &error);
    MARY_ASSERT(r != NULL);
    const char *line = mc_json_string(r, "text");
    MARY_ASSERT(strncmp(line, "cat.png \xE2\x80\x94 image, 6 bytes, ", 27) == 0);
    MARY_ASSERT(mc_json_string(r, "chunk") == NULL);
    MARY_ASSERT_STR(mc_json_string(r, "media_type"), "image");
    MARY_ASSERT(mc_json_bool(mc_json_object(r, "file"), "text_indexed", &indexed) && !indexed);
    json_object_put(r);

    snprintf(path, sizeof path, "%s/Pictures/none.png", home);
    MARY_ASSERT(ix_record(home, "rao", path, &error) == NULL);
    MARY_ASSERT_EQ(error, -ENOENT);
    MARY_ASSERT_STR(ix_relative(home, "/elsewhere/x"), "/elsewhere/x");
    struct json_object *entry = ix_parity_entry("/h/a", 3, 4, "h");
    MARY_ASSERT_STR(mc_json_string(entry, "hash"), "h");
    json_object_put(entry);
    remove_tree(home);
}

int main(void) {
    MARY_RUN(a_text_file_is_chunked_and_a_picture_is_one_line_and_the_hash_is_stable);
    MARY_TEST_MAIN_END();
}
