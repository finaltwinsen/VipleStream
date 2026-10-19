/**
 * @file tests/unit/test_media_share.cpp
 * @brief Test src/media_share.*（§VR-PLAYER V3：路徑不可跑出分享資料夾、HTTP Range 解析與每次回應的長度上限）。
 */
#include "../tests_common.h"

#include <src/media_share.h>

TEST(MediaShareTest, SelfTest) {
  EXPECT_EQ(media_share::self_test(), 0);
}

TEST(MediaShareTest, DisabledByDefault) {
  // 沒有設定 media_dirs 時沒有任何分享資料夾，所有路徑都拒絕
  std::filesystem::path p;
  std::string err;
  EXPECT_FALSE(media_share::resolve(0, "", &p, &err));
}
