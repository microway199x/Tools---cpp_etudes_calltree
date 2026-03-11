//
// Created by grakra on 25-7-29.
//

#include "gtest/gtest.h"
#include "german_string.h"
using starrocks::GermanString;
using starrocks::GermanStringExternalAllocator;
TEST(GermanStringColumnTest, test_german_string_compare) {
    GermanStringExternalAllocator allocator;
    // short vs short
    {
        GermanString gs1("Hello", 5, allocator.allocate(5));
        GermanString gs2("Hello", 5, allocator.allocate(5));
        GermanString gs3("Hello!", 6, allocator.allocate(6));
        GermanString gs4("Helloi", 6, allocator.allocate(6));
        ASSERT_TRUE(gs1 == gs2);
        ASSERT_FALSE(gs1 == gs3);
        ASSERT_FALSE(gs1 == gs4);
        ASSERT_FALSE(gs3 == gs4);
        ASSERT_FALSE(gs3 == gs1);
        ASSERT_FALSE(gs4 == gs1);
        ASSERT_FALSE(gs4 == gs3);

        ASSERT_TRUE(gs1.compare(gs2) == 0);
        ASSERT_TRUE(gs1 < gs3);
        ASSERT_TRUE(gs3 < gs4);
        ASSERT_TRUE(gs4 > gs1);
    }
    // long vs long
    {
        GermanString gs1("Hello GermanString", 18, allocator.allocate(18));
        GermanString gs2("Hello GermanString", 18, allocator.allocate(18));
        GermanString gs3("Hello GermanString!", 19, allocator.allocate(19));
        GermanString gs4("Hello GermanStringi", 19, allocator.allocate(19));
        ASSERT_TRUE(gs1 == gs2);
        ASSERT_FALSE(gs1 == gs3);
        ASSERT_FALSE(gs1 == gs4);
        ASSERT_FALSE(gs3 == gs4);
        ASSERT_FALSE(gs3 == gs1);
        ASSERT_FALSE(gs4 == gs1);
        ASSERT_FALSE(gs4 == gs3);

        ASSERT_TRUE(gs1.compare(gs2) == 0);
        ASSERT_TRUE(gs1 < gs3);
        ASSERT_TRUE(gs3 < gs4);
        ASSERT_TRUE(gs4 > gs1);
    }
    // short vs long
    {
        GermanString gs1("Hello", 5, allocator.allocate(5));
        GermanString gs2("Hello GermanString", 18, allocator.allocate(18));
        GermanString gs3("Hello GermanString!", 19, allocator.allocate(19));
        GermanString gs4("Hello GermanString!", 19, allocator.allocate(19));
        ASSERT_TRUE(gs1 < gs2);
        ASSERT_TRUE(gs1 < gs3);
        ASSERT_TRUE(gs1 < gs4);
        ASSERT_FALSE(gs2 < gs1);
        ASSERT_FALSE(gs3 < gs1);
        ASSERT_FALSE(gs4 < gs1);

        ASSERT_TRUE(gs2 > gs1);
        ASSERT_TRUE(gs3 > gs1);
        ASSERT_TRUE(gs4 > gs1);
    }
}

TEST(GermanStringColumnTest, HashSet) {
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}