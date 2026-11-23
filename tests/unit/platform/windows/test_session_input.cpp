/**
 * @file tests/unit/platform/windows/test_session_input.cpp
 * @brief Tests for moving the mouse and keyboard of a mosaic stream to another session.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <string>

  // local includes
  #include <src/platform/windows/session_input.h>

using platf::session_input::input_target_t;
using platf::session_input::map_to_target;
using platf::session_input::parse_input_target;

TEST(SessionInputTargetTest, ParsesSessionAndPart) {
  const auto t = parse_input_target("# written by OpenStreamMS\r\n3 960 0 960 1080 1920 1080\r\n");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->session, 3u);
  EXPECT_EQ(t->x, 960);
  EXPECT_EQ(t->y, 0);
  EXPECT_EQ(t->width, 960);
  EXPECT_EQ(t->height, 1080);
  EXPECT_EQ(t->canvas_width, 1920);
  EXPECT_EQ(t->canvas_height, 1080);
}

TEST(SessionInputTargetTest, RejectsInvalidContent) {
  EXPECT_FALSE(parse_input_target("").has_value());
  EXPECT_FALSE(parse_input_target("3 960 0 960").has_value());
  EXPECT_FALSE(parse_input_target("0 0 0 960 1080 1920 1080").has_value());
  EXPECT_FALSE(parse_input_target("-2 0 0 960 1080 1920 1080").has_value());
  EXPECT_FALSE(parse_input_target("3 0 0 0 1080 1920 1080").has_value());
  EXPECT_FALSE(parse_input_target("3 1000 0 960 1080 1920 1080").has_value());  // outside the canvas
}

TEST(SessionInputTargetTest, MapsThePartToTheWholeDesktop) {
  const input_target_t right {3, 960, 0, 960, 1080, 1920, 1080};

  auto [x, y] = map_to_target(right, 0.75F, 0.5F);  // centre of the right half
  EXPECT_NEAR(x, 32768, 40);
  EXPECT_NEAR(y, 32768, 40);

  std::tie(x, y) = map_to_target(right, 1.0F, 1.0F);
  EXPECT_EQ(x, 65535);
  EXPECT_EQ(y, 65535);

  // The left half belongs to another session: the pointer stays on the target's edge.
  std::tie(x, y) = map_to_target(right, 0.25F, 0.0F);
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
}

TEST(SessionInputTargetTest, MapsAQuarter) {
  const input_target_t bottom_left {5, 0, 540, 960, 540, 1920, 1080};
  const auto [x, y] = map_to_target(bottom_left, 0.25F, 0.75F);
  EXPECT_NEAR(x, 32768, 40);
  EXPECT_NEAR(y, 32768, 70);
}
#endif
