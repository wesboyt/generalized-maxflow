#include <string>

#include "gmf/json.hpp"
#include "harness.hpp"

using gmf::json::Value;

TEST(scalars_parse_to_their_types) {
  CHECK(gmf::json::parse("null").is_null());
  CHECK(gmf::json::parse("true").as_bool() == true);
  CHECK(gmf::json::parse("false").as_bool() == false);
  CHECK_NEAR(gmf::json::parse("-12.5e2").as_number(), -1250.0, 1e-12);
  CHECK(gmf::json::parse("\"hello\"").as_string() == "hello");
}

TEST(objects_and_arrays_keep_their_shape) {
  const Value value = gmf::json::parse("{\"a\": [1, 2, {\"b\": null}], \"c\": \"d\"}");
  CHECK(value.is_object());
  const Value* a = value.find("a");
  CHECK(a != nullptr);
  CHECK(a->as_array().size() == 3);
  CHECK_NEAR(a->as_array()[1].as_number(), 2.0, 1e-12);
  CHECK(a->as_array()[2].find("b") != nullptr);
  CHECK(a->as_array()[2].find("b")->is_null());
  CHECK(value.find("c")->as_string() == "d");
  CHECK(value.find("missing") == nullptr);
}

TEST(escapes_round_trip) {
  const std::string text = "\"line\\nbreak \\\"quoted\\\" \\u00e9 \\t\"";
  const Value value = gmf::json::parse(text);
  CHECK(value.as_string() == "line\nbreak \"quoted\" \xc3\xa9 \t");
  const Value again = gmf::json::parse(gmf::json::dump(value));
  CHECK(again.as_string() == value.as_string());
}

TEST(surrogate_pairs_become_one_code_point) {
  const Value value = gmf::json::parse("\"\\ud83d\\ude00\"");
  CHECK(value.as_string() == "\xf0\x9f\x98\x80");
}

TEST(malformed_documents_are_rejected) {
  const char* bad[] = {"{", "[1,", "{\"a\" 1}", "tru", "", "{\"a\":1} extra", "\"unterminated"};
  for (const char* text : bad) {
    bool threw = false;
    try {
      gmf::json::parse(text);
    } catch (const gmf::json::ParseError&) {
      threw = true;
    }
    CHECK_MESSAGE(threw, std::string("accepted ") + text);
  }
}

TEST(numbers_print_back_the_way_they_came_in) {
  CHECK(gmf::json::number_to_string(0.0) == "0");
  CHECK(gmf::json::number_to_string(10.0) == "10");
  CHECK(gmf::json::number_to_string(-3.0) == "-3");
  CHECK(gmf::json::number_to_string(0.5) == "0.5");
  CHECK_NEAR(gmf::json::parse(gmf::json::number_to_string(0.1)).as_number(), 0.1, 0.0);
  CHECK_NEAR(gmf::json::parse(gmf::json::number_to_string(1.0 / 3.0)).as_number(), 1.0 / 3.0, 0.0);
}

TEST(infinity_survives_a_round_trip) {
  const double infinity = std::numeric_limits<double>::infinity();
  const std::string text = gmf::json::number_to_string(infinity);
  CHECK(std::isinf(gmf::json::parse(text).as_number()));
  CHECK(std::isinf(gmf::json::parse("\"inf\"").as_number()));
  CHECK(std::isinf(gmf::json::parse("\"Infinity\"").as_number()));
}

TEST(builders_preserve_insertion_order) {
  Value value = Value::object();
  value.set("second", Value(2));
  value.set("first", Value(1));
  value.set("second", Value(3));
  CHECK(gmf::json::dump(value) == "{\"second\":3,\"first\":1}");

  Value list = Value::array();
  list.push_back(Value("a"));
  list.push_back(Value(1));
  CHECK(gmf::json::dump(list) == "[\"a\",1]");
}

TEST(pretty_printing_indents) {
  const Value value = gmf::json::parse("{\"a\":[1]}");
  CHECK(gmf::json::dump(value, 2) == "{\n  \"a\": [\n    1\n  ]\n}");
}

TEST(aliases_resolve_to_the_first_present_key) {
  const Value value = gmf::json::parse("{\"gain\": 2}");
  const Value* found = value.find_any({"multiplier", "gain", "mult"});
  CHECK(found != nullptr);
  CHECK_NEAR(found->as_number(), 2.0, 1e-12);
  CHECK(value.find_any({"nope", "nothing"}) == nullptr);
}
