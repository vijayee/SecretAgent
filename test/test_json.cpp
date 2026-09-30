//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/Util/json.h"
}

TEST(TestJson, TestRoundTripFlatObject) {
  json_value_t* o = json_new_object();
  json_object_set(o, "seq", json_new_int(42));
  json_object_set(o, "type", json_new_string("cell.result"));
  json_object_set(o, "ok", json_new_bool(1));
  json_object_set(o, "empty", json_new_null());
  char* text = json_serialize(o);

  char* err = NULL;
  json_value_t* back = json_parse(text, strlen(text), &err);
  ASSERT_NE(back, nullptr) << (err ? err : "no error message");
  EXPECT_EQ(json_as_int(json_get(back, "seq")), 42);
  EXPECT_STREQ(json_as_string(json_get(back, "type")), "cell.result");
  EXPECT_EQ(json_as_bool(json_get(back, "ok")), 1);
  EXPECT_EQ(json_type(json_get(back, "empty")), JSON_NULL);

  free(text);
  json_value_destroy(o);
  json_value_destroy(back);
}

TEST(TestJson, TestParseMalformedIsNotNullNoCrash) {
  const char* bad[] = {"{\"a\":}", "{\"a\" 1}", "[1, 2,]", "{\"a\":\"\\x\"}"};
  for (const char* text : bad) {
    char* err = NULL;
    json_value_t* v = json_parse(text, strlen(text), &err);
    EXPECT_EQ(v, nullptr) << "input: " << text;
    if (err) free(err);
  }
}

TEST(TestJson, TestEscapesAndUnicode) {
  const char* in = "{\"s\":\"line\\nbreak \\u00e9 \\\"q\\\" \\\\\"}";
  char* err = NULL;
  json_value_t* v = json_parse(in, strlen(in), &err);
  ASSERT_NE(v, nullptr);
  const char* s = json_as_string(json_get(v, "s"));
  ASSERT_NE(s, nullptr);
  EXPECT_NE(strstr(s, "\n"), nullptr);
  EXPECT_NE(strstr(s, "é"), nullptr);
  json_value_destroy(v);
}

TEST(TestJson, TestArraysAndNesting) {
  const char* in = "{\"tools\":[\"a\",\"b\"],\"ctx\":{\"depth\":2},\"rows\":[{\"n\":1},{\"n\":2}]}";
  char* err = NULL;
  json_value_t* v = json_parse(in, strlen(in), &err);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(json_size(json_get(v, "tools")), 2u);
  EXPECT_STREQ(json_as_string(json_at(json_get(v, "tools"), 1)), "b");
  EXPECT_EQ(json_as_int(json_get(json_get(v, "ctx"), "depth")), 2);
  ASSERT_EQ(json_size(json_get(v, "rows")), 2u);
  json_value_destroy(v);
}