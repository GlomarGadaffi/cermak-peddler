// PageZone_test.cpp — unit coverage for paging-zone PBX helpers in PbxConfig.hpp.
// These are pure inline functions; no socket linkage required.

#include <gtest/gtest.h>
#include "PbxConfig.hpp"
#include "PoolConfig.hpp"

#include <string>

// ── isPageZoneExt ─────────────────────────────────────────────────────────────

TEST(PageZone, ExtInRange980to989) {
    EXPECT_TRUE(pbx::isPageZoneExt("980"));
    EXPECT_TRUE(pbx::isPageZoneExt("981"));
    EXPECT_TRUE(pbx::isPageZoneExt("985"));
    EXPECT_TRUE(pbx::isPageZoneExt("989"));
}

TEST(PageZone, ExtOutOfRange) {
    EXPECT_FALSE(pbx::isPageZoneExt("979"));  // below range (ext[1] == '7')
    EXPECT_FALSE(pbx::isPageZoneExt("990"));  // ext[1] == '9', not '8'
    EXPECT_FALSE(pbx::isPageZoneExt("999"));
    EXPECT_FALSE(pbx::isPageZoneExt("777"));
}

TEST(PageZone, ExtWrongLength) {
    EXPECT_FALSE(pbx::isPageZoneExt("98"));    // too short
    EXPECT_FALSE(pbx::isPageZoneExt("9800"));  // too long
    EXPECT_FALSE(pbx::isPageZoneExt(""));
}

// ── splitZoneMembers ──────────────────────────────────────────────────────────

TEST(PageZone, SplitMembersBasic) {
    auto m = pbx::splitZoneMembers("100,101,102");
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m[0], "100");
    EXPECT_EQ(m[1], "101");
    EXPECT_EQ(m[2], "102");
}

TEST(PageZone, SplitMembersDeduplicates) {
    auto m = pbx::splitZoneMembers("100,101,100");
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0], "100");
    EXPECT_EQ(m[1], "101");
}

TEST(PageZone, SplitMembersEmptyYieldsNone) {
    EXPECT_TRUE(pbx::splitZoneMembers("").empty());
    EXPECT_TRUE(pbx::splitZoneMembers("  , ,  ").empty());
}

TEST(PageZone, SplitMembersCappedAtZoneMemberCap) {
    std::string csv;
    for (int i = 0; i < POCKETDIAL_ZONE_MEMBER_CAP * 2; ++i) {
        if (i) csv += ',';
        csv += std::to_string(1000 + i);
    }
    auto m = pbx::splitZoneMembers(csv);
    EXPECT_LE(static_cast<int>(m.size()), POCKETDIAL_ZONE_MEMBER_CAP);
}

// ── joinMembers ───────────────────────────────────────────────────────────────

TEST(PageZone, JoinMembersBasic) {
    EXPECT_EQ(pbx::joinMembers({"100", "101", "102"}), "100,101,102");
}

TEST(PageZone, JoinMembersEmpty) {
    EXPECT_EQ(pbx::joinMembers({}), "");
}

TEST(PageZone, JoinMembersSingle) {
    EXPECT_EQ(pbx::joinMembers({"100"}), "100");
}

TEST(PageZone, JoinThenSplitRoundTrip) {
    std::vector<std::string> orig = {"100", "101", "105"};
    auto joined = pbx::joinMembers(orig);
    auto split  = pbx::splitZoneMembers(joined);
    ASSERT_EQ(split.size(), orig.size());
    for (size_t i = 0; i < orig.size(); ++i)
        EXPECT_EQ(split[i], orig[i]);
}
