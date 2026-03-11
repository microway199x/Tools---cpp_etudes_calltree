// Copyright (c) 2020 Ran Panfeng.  All rights reserved.
// Author: satanson
// Email: ranpanf@gmail.com
// Github repository: https://github.com/satanson/cpp_etudes.git

//
// Created by grakra on 2020/9/23.
//

#include <gtest/gtest.h>
#include <immintrin.h>
#include <math.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <guard.hh>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <random>

using namespace std;
class CudaTest : public ::testing::Test {};

struct WithProperties {
    // Expands to
    unsigned int x;
    unsigned int __fetch_builtin_x(void) { return 0; }

    unsigned int y;
    unsigned int __fetch_builtin_y(void) { return 0; }
};
TEST_F(CudaTest, test_properties) {
    WithProperties wp;
    EXPECT_EQ(wp.x, 0);
    EXPECT_EQ(wp.y, 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}