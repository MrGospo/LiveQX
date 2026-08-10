// fix30 c2 — Preloader::computeWarmSet logic. Pure function, no Timeline
// or render thread; covers wrap-around, clamping, and short-playlist cases.

#include <gtest/gtest.h>

#include <vector>

#include "core/Preloader.h"

TEST(PreloaderWarmSet, EmptyPlaylist) {
    EXPECT_TRUE(Preloader::computeWarmSet(0, 0, 2).empty());
}

TEST(PreloaderWarmSet, ZeroOrNegativeCap) {
    EXPECT_TRUE(Preloader::computeWarmSet(5, 0,  0).empty());
    EXPECT_TRUE(Preloader::computeWarmSet(5, 0, -1).empty());
}

TEST(PreloaderWarmSet, SingleClip) {
    EXPECT_EQ(Preloader::computeWarmSet(1, 0, 2), (std::vector<int>{0}));
}

TEST(PreloaderWarmSet, EqualsPlaylistWhenSmall) {
    EXPECT_EQ(Preloader::computeWarmSet(2, 0, 2), (std::vector<int>{0, 1}));
    EXPECT_EQ(Preloader::computeWarmSet(2, 1, 2), (std::vector<int>{1, 0}));
}

TEST(PreloaderWarmSet, SlidesOnLongPlaylist) {
    EXPECT_EQ(Preloader::computeWarmSet(5, 2, 2), (std::vector<int>{2, 3}));
}

TEST(PreloaderWarmSet, WrapsAround) {
    EXPECT_EQ(Preloader::computeWarmSet(3, 2, 2), (std::vector<int>{2, 0}));
    EXPECT_EQ(Preloader::computeWarmSet(5, 4, 3), (std::vector<int>{4, 0, 1}));
}

TEST(PreloaderWarmSet, ClampsLargeCurIdx) {
    EXPECT_EQ(Preloader::computeWarmSet(3, 7, 2), (std::vector<int>{1, 2}));
}

TEST(PreloaderWarmSet, FoldsNegativeCurIdx) {
    EXPECT_EQ(Preloader::computeWarmSet(3, -1, 2), (std::vector<int>{2, 0}));
    EXPECT_EQ(Preloader::computeWarmSet(3, -4, 2), (std::vector<int>{2, 0}));
}

TEST(PreloaderWarmSet, CapAboveSizeReturnsAtMostSize) {
    EXPECT_EQ(Preloader::computeWarmSet(2, 0, 5), (std::vector<int>{0, 1}));
}

TEST(PreloaderWarmSet, ActiveAlwaysFirst) {
    for (int n = 1; n <= 8; ++n) {
        for (int cur = 0; cur < n; ++cur) {
            const auto s = Preloader::computeWarmSet(n, cur, 3);
            ASSERT_FALSE(s.empty()) << "n=" << n << " cur=" << cur;
            EXPECT_EQ(s.front(), cur)
                << "active must be first; n=" << n << " cur=" << cur;
        }
    }
}
