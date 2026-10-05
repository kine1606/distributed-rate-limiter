#include <gtest/gtest.h>

#include "core/allow_all_core.h"

TEST(AllowAllCore, CompletesInlineAndAllows) {
  ratelimiter::AllowAllCore core;
  bool called = false;
  core.Check({"user:1", "api_default", 1}, [&](const ratelimiter::CheckResult& r) {
    called = true;
    EXPECT_TRUE(r.allowed);
  });
  EXPECT_TRUE(called);
}
