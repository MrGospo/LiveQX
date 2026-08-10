#include <gtest/gtest.h>
#include <stdexcept>
#include "clips/ClipFactory.h"

// Image clips: файлы не открываются при create(), prepare() вызывается отдельно.
TEST(ClipFactoryTest, ImageExtensionsProduceNonNull) {
    for (const char* path : { "photo.jpg", "a.jpeg", "b.PNG", "c.bmp", "d.webp" }) {
        PlaylistItem item{ path, 5.0, {} };
        auto clip = ClipFactory::create(item, 1280, 720);
        EXPECT_NE(clip, nullptr) << "failed for: " << path;
    }
}

// Video clips: MediaValidator открывает файл при create().
// Несуществующие файлы → nullptr (валидатор отклоняет их).
TEST(ClipFactoryTest, NonExistentVideoReturnsNull) {
    for (const char* path : { "video.mp4", "v.avi", "v.mkv", "v.mov", "v.ts" }) {
        PlaylistItem item{ path, 0.0, {} };
        auto clip = ClipFactory::create(item, 1280, 720);
        EXPECT_EQ(clip, nullptr) << "expected nullptr for non-existent: " << path;
    }
}

// Реальный тестовый файл → не nullptr.
TEST(ClipFactoryTest, RealVideoFileProducesNonNull) {
    PlaylistItem item{ "test_media/clip.mp4", 0.0, {} };
    auto clip = ClipFactory::create(item, 1280, 720);
    EXPECT_NE(clip, nullptr);
}

TEST(ClipFactoryTest, UnknownExtensionThrows) {
    PlaylistItem item{ "file.xyz", 5.0, {} };
    EXPECT_THROW(ClipFactory::create(item, 1280, 720), std::runtime_error);
}

TEST(ClipFactoryTest, ExtensionCaseInsensitive) {
    PlaylistItem item{ "PHOTO.JPG", 5.0, {} };
    EXPECT_NE(ClipFactory::create(item, 1280, 720), nullptr);
}

TEST(ClipFactoryTest, DisplayDurationPassedToImageClip) {
    PlaylistItem item{ "photo.jpg", 7.5, {} };
    auto clip = ClipFactory::create(item, 640, 480);
    ASSERT_NE(clip, nullptr);
    EXPECT_DOUBLE_EQ(clip->getDuration(), 7.5);
}
