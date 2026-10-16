// audit/AuditTypes — the category list is one source of truth.
//
// Several places need "every category" (the retention worker, the
// /api/audit/categories endpoint). When each kept its own hand-written list,
// a new category could be added to the enum and silently missing from one of
// them. These tests pin the shared list.

#include "audit/AuditTypes.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace liveqx::audit;

TEST(AuditTypes, EveryListedCategoryRoundTripsThroughItsName) {
    for (Category c : kAllCategories) {
        const std::string name = categoryName(c);
        ASSERT_FALSE(name.empty());
        auto back = categoryFromString(name);
        ASSERT_TRUE(back.has_value()) << name;
        EXPECT_EQ(*back, c) << name;
    }
}

TEST(AuditTypes, CategoryNamesAreUniqueAndStable) {
    std::set<std::string> seen;
    for (Category c : kAllCategories) EXPECT_TRUE(seen.insert(categoryName(c)).second) << categoryName(c);
    // Operators write SIEM filters on these strings: they must not change.
    for (const char* n : {"auth", "channel", "output", "gateway", "plugin", "mount", "system", "access"})
        EXPECT_TRUE(seen.count(n)) << n << " must stay";
}

TEST(AuditTypes, ThePlayerCategoryExists) {
    EXPECT_STREQ(categoryName(Category::Player), "player");
    EXPECT_EQ(categoryFromString("player"), Category::Player);
    bool listed = false;
    for (Category c : kAllCategories) listed |= (c == Category::Player);
    EXPECT_TRUE(listed) << "retention and the categories endpoint iterate this list";
}

TEST(AuditTypes, PlayerEventsAreKeptAsLongAsAuthEvents) {
    // Pairing and revoking a device is an access decision: same regulator
    // baseline as logins.
    EXPECT_EQ(defaultRetentionDays(Category::Player), defaultRetentionDays(Category::Auth));
}

TEST(AuditTypes, EveryCategoryHasARetention) {
    for (Category c : kAllCategories) EXPECT_GT(defaultRetentionDays(c), 0) << categoryName(c);
}

TEST(AuditTypes, AnUnknownNameIsNotACategory) {
    EXPECT_FALSE(categoryFromString("players").has_value());
    EXPECT_FALSE(categoryFromString("").has_value());
    EXPECT_FALSE(categoryFromString("Player").has_value()) << "names are lower case";
}
