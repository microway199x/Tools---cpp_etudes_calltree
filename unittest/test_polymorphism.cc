// Copyright (c) 2020 Ran Panfeng.  All rights reserved.
// Author: satanson
// Email: ranpanf@gmail.com
// Github repository: https://github.com/satanson/cpp_etudes.git

//
// Created by grakra on 2020/7/2.
//
#include <absl/container/flat_hash_map.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <immintrin.h>
#include <mmintrin.h>

#include <random>
#include <util/bits_op.hh>

namespace com {
namespace grakra {
namespace util {

class TestUtil : public ::testing::Test {};

template <typename ArgType, typename ReturnType, typename... ExtraArgs>
struct Concept {
    virtual ReturnType run(ArgType arg, ExtraArgs&&... args) = 0;
};

template <typename ArgType, typename T, typename ReturnType, typename... ExtraArgs>
struct Model : public Concept<ArgType, ReturnType, ExtraArgs...> {
    explicit Model(T&& t) : _t(std::move(t)) {}
    ReturnType run(ArgType arg, ExtraArgs&&... args) override {
        return _t.run(arg, std::forward<ExtraArgs&&>(args)...);
    }

private:
    T _t;
};

using ConceptA = Concept<int, int, int>;
using ConceptB = Concept<int, std::string, int>;

template <typename T>
using ModelA = Model<int, T, int, int>;

template <typename T>
using ModelB = Model<int, T, std::string, int>;

struct A1 {
    int run(int arg, int extra) { return arg + extra; }
};
struct A2 {
    int run(int arg, int extra) { return arg * extra; }
};
struct B1 {
    std::string run(int arg, int extra) { return std::to_string(arg + extra); }
};

struct B2 {
    std::string run(int arg, int extra) { return std::to_string(arg * extra); }
};
TEST_F(TestUtil, testConceptModel) {
    std::vector<std::shared_ptr<ConceptA>> conceptA_vec;
    std::vector<std::shared_ptr<ConceptB>> conceptB_vec;
    conceptA_vec.emplace_back(std::shared_ptr<ConceptA>(new ModelA<A1>(A1())));
    conceptA_vec.emplace_back(std::shared_ptr<ConceptA>(new ModelA<A2>(A2())));
    conceptB_vec.emplace_back(std::shared_ptr<ConceptB>(new ModelB<B1>(B1())));
    conceptB_vec.emplace_back(std::shared_ptr<ConceptB>(new ModelB<B2>(B2())));
    for (auto& con : conceptA_vec) {
        std::cout<<con->run(10,20) << std::endl;
    }
    for (auto& con : conceptB_vec) {
        std::cout<<con->run(10,20) << std::endl;
    }
}

} // namespace util
} // namespace grakra
} // namespace com

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
