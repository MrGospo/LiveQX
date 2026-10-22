// utils/TextFold — case-insensitive search over UTF-8 without a locale.
//
// What these tests protect: capitals and small letters of Latin and Cyrillic
// meet (including Ё and the Ukrainian letters), everything else is left exactly
// as it is, and a broken byte sequence never loses or invents a character.

#include "utils/TextFold.h"

#include <gtest/gtest.h>

using liveqx::text::foldForSearch;

TEST(TextFold, LatinCapitalsBecomeSmall) {
    EXPECT_EQ(foldForSearch("Till 4 ABC xyz"), "till 4 abc xyz");
}

TEST(TextFold, CyrillicCapitalsBecomeSmallIncludingYoAndTheUkrainianLetters) {
    EXPECT_EQ(foldForSearch("КАССА Витрина"), "касса витрина");
    EXPECT_EQ(foldForSearch("Ёлка"), "ёлка");
    EXPECT_EQ(foldForSearch("ЄІЇ"), "єії");
    EXPECT_EQ(foldForSearch("Ж"), "ж");
}

TEST(TextFold, TextThatIsAlreadySmallAndOtherAlphabetsAreLeftAlone) {
    EXPECT_EQ(foldForSearch("касса"), "касса");
    EXPECT_EQ(foldForSearch("日本語 Ünï"), "日本語 Ünï") << "not folded, and said so in the header";
    EXPECT_EQ(foldForSearch(""), "");
}

TEST(TextFold, TwoSpellingsOfOneNameMeet) {
    EXPECT_EQ(foldForSearch("Касса 1"), foldForSearch("касса 1"));
    EXPECT_NE(foldForSearch("Касса 1"), foldForSearch("касса 2"));
}

TEST(TextFold, BrokenUtf8PassesThroughByteByByte) {
    const std::string lone_lead = "A\xD0";                 // a lead byte with nothing after it
    EXPECT_EQ(foldForSearch(lone_lead), "a\xD0");
    const std::string stray = "\x80Z";                      // a continuation byte on its own
    EXPECT_EQ(foldForSearch(stray), "\x80z");
    EXPECT_EQ(foldForSearch(std::string("\xD0") + "A").size(), 2u) << "a lead byte followed by ASCII is not swallowed";
}
