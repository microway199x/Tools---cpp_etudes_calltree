// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "raw_container.hh"

namespace starrocks {

class GermanStringExternalAllocator {
public:
    static constexpr auto PAGE_SIZE = 4096;
    static constexpr auto MEDIUM_STRING_MAX_SIZE = 512;
    size_t size() const;

    void clear();

    char* allocate(size_t n);

private:
    std::vector<std::vector<char>> medium_string_pages;
    std::vector<std::string> large_strings;
};

static inline int memcompare(const char* p1, size_t size1, const char* p2, size_t size2) {
    size_t min_size = std::min(size1, size2);
    auto res = memcmp(p1, p2, min_size);
    if (res != 0) {
        return res > 0 ? 1 : -1;
    }
    return size1 < size2 ? -1 : (size1 > size2 ? 1 : 0);
}

// A GermanString is a string that can be stored inline or as a pointer to a larger buffer.

class GermanString {
public:
    static constexpr uint32_t INLINE_MAX_LENGTH = 12;
    static constexpr uint32_t PREFIX_LENGTH = 4;
    GermanString();
    GermanString(const char* str, size_t len, char* remaining);
    GermanString(const GermanString& rhs, char* remaining);
    GermanString(const GermanString& rhs);
    GermanString(const std::string_view& str, char* remaining) : GermanString(str.data(), str.size(), remaining) {};

    explicit operator std::string() const;
    void append(const char* str, size_t len, char* remaining);
    void append(const std::string_view& str, char* remaining) { append(str.data(), str.size(), remaining); }
    bool is_inline() const { return len <= INLINE_MAX_LENGTH; }
    int compare(const GermanString& rhs) const;

    inline bool operator==(const GermanString& rhs) const {
        if (this->is_inline() || rhs.is_inline()) {
            auto p0 = reinterpret_cast<const char*>(this);
            auto p1 = reinterpret_cast<const char*>(&rhs);
            const auto sz = sizeof(GermanString);
            return memcompare(p0, sz, p1, sz) == 0;
        } else {
            return memcompare(this->long_rep.prefix, PREFIX_LENGTH, rhs.long_rep.prefix, PREFIX_LENGTH) == 0 &&
                   memcompare(reinterpret_cast<const char*>(this->long_rep.ptr), this->len - PREFIX_LENGTH,
                              reinterpret_cast<const char*>(rhs.long_rep.ptr), rhs.len - PREFIX_LENGTH) == 0;
        }
    }
    inline bool operator!=(const GermanString& rhs) const { return !(*this == rhs); }
    inline bool operator<(const GermanString& rhs) const { return compare(rhs) < 0; }
    inline bool operator<=(const GermanString& rhs) const { return compare(rhs) <= 0; }
    inline bool operator>(const GermanString& rhs) const { return compare(rhs) > 0; }
    inline bool operator>=(const GermanString& rhs) const { return compare(rhs) >= 0; }
    inline bool operator<=>(const GermanString& rhs) const { return compare(rhs); }

    union {
        uint32_t len;
        struct {
            uint32_t len;
            char str[INLINE_MAX_LENGTH];
        } short_rep;
        struct {
            uint32_t len;
            char prefix[PREFIX_LENGTH];
            uintptr_t ptr;
        } long_rep;
    };
};

size_t GermanStringExternalAllocator::size() const {
    size_t total_size = 0;
    for (const auto& page : medium_string_pages) {
        total_size += page.size();
    }
    for (const auto& str : large_strings) {
        total_size += str.size();
    }
    return total_size;
}

void GermanStringExternalAllocator::clear() {
    large_strings.clear();
    medium_string_pages.clear();
}

char* GermanStringExternalAllocator::allocate(size_t n) {
    if (n <= GermanString::INLINE_MAX_LENGTH) {
        return nullptr;
    }
    n -= GermanString::PREFIX_LENGTH; // reserve 4 bytes for length prefix
    if (n <= MEDIUM_STRING_MAX_SIZE) {
        if (medium_string_pages.empty() || medium_string_pages.back().size() + n > PAGE_SIZE) {
            medium_string_pages.emplace_back();
            medium_string_pages.back().reserve(PAGE_SIZE);
        }
        auto& page = medium_string_pages.back();
        page.resize(page.size() + n);
        return page.data() + page.size() - n;
    } else {
        large_strings.emplace_back();
        auto& str = large_strings.back();
        raw::make_room(str, n);
        return large_strings.back().data();
    }
}

GermanString::GermanString() {
    auto* p = reinterpret_cast<char*>(this);
    std::fill(p, p + sizeof(GermanString), 0);
}

GermanString::GermanString(const starrocks::GermanString& rhs) {
    memcpy(this, &rhs, sizeof(GermanString));
}

GermanString::GermanString(const char* str, size_t len, char* remaining) {
    this->len = len;
    if (len <= INLINE_MAX_LENGTH) {
        long_rep.ptr = 0;
        memcpy(short_rep.str, str, len);
    } else {
        memcpy(long_rep.prefix, str, PREFIX_LENGTH);
        memcpy(remaining, str + PREFIX_LENGTH, len - PREFIX_LENGTH);
        long_rep.ptr = reinterpret_cast<uintptr_t>(remaining);
    }
}

GermanString::GermanString(const GermanString& rhs, char* remaining) {
    if (rhs.len <= INLINE_MAX_LENGTH) {
        long_rep.ptr = 0;
        memcpy(reinterpret_cast<char*>(this), reinterpret_cast<const char*>(&rhs), sizeof(GermanString));
    } else {
        memcpy(reinterpret_cast<char*>(this), reinterpret_cast<const char*>(&rhs),
               sizeof(GermanString) - sizeof(uintptr_t));
        const auto* rhs_ptr = reinterpret_cast<const char*>(rhs.long_rep.ptr);
        memcpy(remaining, rhs_ptr, rhs.len - PREFIX_LENGTH);
        long_rep.ptr = reinterpret_cast<uintptr_t>(remaining);
    }
}

int GermanString::compare(const GermanString& rhs) const {
    if (is_inline() && rhs.is_inline()) {
        return memcompare(short_rep.str, len, rhs.short_rep.str, rhs.len);
    } else if (this->is_inline()) {
        auto min_len = std::min(this->len, PREFIX_LENGTH);
        auto r = memcompare(short_rep.str, min_len, rhs.long_rep.prefix, min_len);
        if (r != 0) {
            return r;
        } else if (min_len <= PREFIX_LENGTH) {
            return -1;
        } else {
            return memcompare(short_rep.str + PREFIX_LENGTH, len - PREFIX_LENGTH,
                              reinterpret_cast<const char*>(rhs.long_rep.ptr), rhs.len - PREFIX_LENGTH);
        }
    } else if (rhs.is_inline()) {
        auto min_len = std::min(rhs.len, PREFIX_LENGTH);
        auto r = memcompare(long_rep.prefix, min_len, rhs.short_rep.str, min_len);
        if (r != 0) {
            return r;
        } else if (min_len <= PREFIX_LENGTH) {
            return 1;
        } else {
            return memcompare(reinterpret_cast<const char*>(long_rep.ptr), len - PREFIX_LENGTH,
                              rhs.short_rep.str + PREFIX_LENGTH, rhs.len - PREFIX_LENGTH);
        }
    } else {
        auto r = memcompare(long_rep.prefix, PREFIX_LENGTH, rhs.long_rep.prefix, PREFIX_LENGTH);
        if (r != 0) {
            return r;
        }
        auto* this_ptr = reinterpret_cast<const char*>(long_rep.ptr);
        auto* rhs_ptr = reinterpret_cast<const char*>(rhs.long_rep.ptr);
        return memcompare(this_ptr, len - PREFIX_LENGTH, rhs_ptr, rhs.len - PREFIX_LENGTH);
    }
}

void GermanString::append(const char* str, size_t len, char* remaining) {
    auto old_len = this->len;
    this->len += len;
    if (len <= INLINE_MAX_LENGTH) {
        memcpy(short_rep.str + old_len, str, len);
    } else if (old_len <= INLINE_MAX_LENGTH) {
        auto remaining_front_len = old_len - PREFIX_LENGTH;
        char* remaining_front = reinterpret_cast<char*>(long_rep.ptr);
        memcpy(remaining, remaining_front, remaining_front_len);
        memcpy(remaining + remaining_front_len, str, len);
        long_rep.ptr = reinterpret_cast<uintptr_t>(remaining);
    } else {
        memcpy(remaining, str, len);
    }
}

GermanString::operator std::string() const {
    if (len <= INLINE_MAX_LENGTH) {
        return std::string(short_rep.str, len);
    } else {
        std::string s;
        raw::make_room(s, len);
        char* data = s.data();
        memcpy(data, long_rep.prefix, PREFIX_LENGTH);
        memcpy(data + PREFIX_LENGTH, reinterpret_cast<const char*>(long_rep.ptr), len - PREFIX_LENGTH);
        return s;
    }
}

} // namespace starrocks

namespace std {
static inline std::string to_string(const starrocks::GermanString& gs) {
    return static_cast<std::string>(gs);
}

} // namespace std
