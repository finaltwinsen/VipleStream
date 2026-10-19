/**
 * @file tests/unit/test_media_share.cpp
 * @brief Test src/media_share.*（§VR-PLAYER V3：路徑不可跑出分享資料夾、HTTP Range 解析與每次回應的長度上限）。
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/media_share.h>

TEST(MediaShareTest, SelfTest) {
  EXPECT_EQ(media_share::self_test(), 0);
}

TEST(MediaShareTest, WholePcByDefault) {
  // 沒設 media_dirs＝整台 PC：每個根都是磁碟機（Linux 是 /），從根出發的 .. 一律拒絕
  const auto roots = media_share::roots();
  ASSERT_FALSE(roots.empty());
  for (const auto &r : roots) {
    EXPECT_TRUE(r.drive);
  }
  std::filesystem::path p;
  std::string err;
  EXPECT_TRUE(media_share::resolve(roots[0].index, "", &p, &err));
  EXPECT_FALSE(media_share::resolve(roots[0].index, "..", &p, &err));
  EXPECT_FALSE(media_share::resolve(roots[0].index, "a/../../b", &p, &err));
}

TEST(MediaShareTest, Disabled) {
  // media_share = disabled：沒有任何根，所有路徑都拒絕
  const bool saved = config::nvhttp.media_share;
  config::nvhttp.media_share = false;
  EXPECT_TRUE(media_share::roots().empty());
  std::filesystem::path p;
  std::string err;
  EXPECT_FALSE(media_share::resolve(0, "", &p, &err));
  config::nvhttp.media_share = saved;
}
