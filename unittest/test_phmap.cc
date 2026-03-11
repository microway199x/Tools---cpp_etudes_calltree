//
// Created by grakra on 2025/11/24.
//


#include <glog/logging.h>

#include "gtest/gtest.h"
#include "phmap/phmap_hash.h"
#include "phmap/phmap_base.h"
#include "phmap/phmap_bits.h"
#include "phmap/phmap.h"
#include <unordered_set>
#if defined(__SANITIZE_ADDRESS__) || defined(ADDRESS_SANITIZER)

// Marks memory region [addr, addr+size) as unaddressable.
// This memory must be previously allocated by the user program. Accessing
// addresses in this region from instrumented code is forbidden until
// this region is unpoisoned. This function is not guaranteed to poison
// the whole region - it may poison only subregion of [addr, addr+size) due
// to ASan alignment restrictions.
// Method is NOT thread-safe in the sense that no two threads can
// (un)poison memory in the same memory region simultaneously.
extern "C" void __asan_poison_memory_region(void const volatile* addr, size_t size);
// Marks memory region [addr, addr+size) as addressable.
// This memory must be previously allocated by the user program. Accessing
// addresses in this region is allowed until this region is poisoned again.
// This function may unpoison a superregion of [addr, addr+size) due to
// ASan alignment restrictions.
// Method is NOT thread-safe in the sense that no two threads can
// (un)poison memory in the same memory region simultaneously.
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, size_t size);

#define SR_ASAN_POISON_MEMORY_REGION(addr, size) __asan_poison_memory_region((addr), (size))
#define SR_ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#define SR_ASAN_POISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#define SR_ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif
#define ALWAYS_INLINE __attribute__((always_inline))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
struct MemChunk {
    uint8_t* data = nullptr;
    size_t size;
    int core_id;
};

static inline int64_t RoundUpToPowerOf2(int64_t value, int64_t factor) {
    DCHECK((factor > 0) && ((factor & (factor - 1)) == 0));
    return (value + (factor - 1)) & ~(factor - 1);
}

class MemChunkAllocator {
public:
    static bool allocate(size_t size, MemChunk* chunk) {
        auto* p = new char[size];  // allocate memory using new
        if (p== nullptr) {
            return false;
        }
        chunk->data = reinterpret_cast<uint8_t *>(p);
        chunk->size = size;
        //chunk->core_id = 0;
        return true;
    }

    static void free(const MemChunk& chunk) {
        delete[] chunk.data;
    }
};

class MemPool {
public:
    MemPool() : next_chunk_size_(INITIAL_CHUNK_SIZE) {}

    /// Frees all chunks of memory and subtracts the total allocated bytes
    /// from the registered limits.
    ~MemPool();

    /// Allocates a section of memory of 'size' bytes with DEFAULT_ALIGNMENT at the end
    /// of the the current chunk. Creates a new chunk if there aren't any chunks
    /// with enough capacity.
    uint8_t* allocate(int64_t size) { return allocate<false>(size, DEFAULT_ALIGNMENT, 0); }

    uint8_t* allocate_with_reserve(int64_t size, int reserve) {
        return allocate<false>(size, DEFAULT_ALIGNMENT, reserve);
    }

    // Don't check memory limit
    uint8_t* allocate_aligned(int64_t size, int alignment) {
        DCHECK_GE(alignment, 1);
        DCHECK_LE(alignment, 16);
        // alignment should be a power of 2
        DCHECK((alignment & (alignment - 1)) == 0);
        return allocate<false>(size, alignment, 0);
    }

    /// Makes all allocated chunks available for re-use, but doesn't delete any chunks.
    void clear();

    /// Deletes all allocated chunks. free_all() or acquire_data() must be called for
    /// each mem pool
    void free_all();

    /// Absorb all chunks that hold data from src. If keep_current is true, let src hold on
    /// to its last allocated chunk that contains data.
    /// All offsets handed out by calls to GetCurrentOffset() for 'src' become invalid.
    void acquire_data(MemPool* src, bool keep_current);

    // Exchange all chunks with input source, including reserved chunks.
    // This function will keep its own MemTracker, and update it after exchange.
    // Why we need this other than std::swap? Because swap will swap MemTracker too, which would
    // lead error. We only has MemTracker's pointer, which can be invalid after swap.
    void exchange_data(MemPool* other);

    std::string debug_string();

    int64_t total_allocated_bytes() const { return total_allocated_bytes_; }
    int64_t total_reserved_bytes() const { return total_reserved_bytes_; }
    int64_t peak_allocated_bytes() const { return peak_allocated_bytes_; }

    static const int DEFAULT_ALIGNMENT = 16;

private:
    friend class MemPoolTest;
    static const int INITIAL_CHUNK_SIZE = 4 * 1024;

    /// The maximum size of chunk that should be allocated. Allocations larger than this
    /// size will get their own individual chunk.
    static const int MAX_CHUNK_SIZE = 512 * 1024;

    struct ChunkInfo {
        MemChunk chunk;
        /// bytes allocated via Allocate() in this chunk
        int64_t allocated_bytes{0};
        explicit ChunkInfo(const MemChunk& chunk);
        ChunkInfo() = default;
    };

    /// A static field used as non-NULL pointer for zero length allocations. NULL is
    /// reserved for allocation failures. It must be as aligned as max_align_t for
    /// TryAllocateAligned().
    static uint32_t k_zero_length_region_;

    /// Find or allocated a chunk with at least min_size spare capacity and update
    /// current_chunk_idx_. Also updates chunks_, chunk_sizes_ and allocated_bytes_
    /// if a new chunk needs to be created.
    /// If check_limits is true, this call can fail (returns false) if adding a
    /// new chunk exceeds the mem limits.
    bool find_chunk(size_t min_size, bool check_limits);

    /// Check integrity of the supporting data structures; always returns true but DCHECKs
    /// all invariants.
    /// If 'check_current_chunk_empty' is true, checks that the current chunk contains no
    /// data. Otherwise the current chunk can be either empty or full.
    bool check_integrity(bool check_current_chunk_empty);

    /// Return offset to unoccupied space in current chunk.
    int64_t get_free_offset() const {
        if (current_chunk_idx_ == -1) return 0;
        return chunks_[current_chunk_idx_].allocated_bytes;
    }

    template <bool CHECK_LIMIT_FIRST>
    uint8_t* ALWAYS_INLINE allocate(int64_t size, int alignment, int reserve) {
        DCHECK_GE(size, 0);
        if (UNLIKELY(size == 0)) return reinterpret_cast<uint8_t*>(&k_zero_length_region_);

        if (current_chunk_idx_ != -1) {
            ChunkInfo& info = chunks_[current_chunk_idx_];
            int64_t aligned_allocated_bytes = RoundUpToPowerOf2(info.allocated_bytes, alignment);
            if (aligned_allocated_bytes + size + reserve <= info.chunk.size) {
                // Ensure the requested alignment is respected.
                int64_t padding = aligned_allocated_bytes - info.allocated_bytes;
                uint8_t* result = info.chunk.data + aligned_allocated_bytes;
                SR_ASAN_UNPOISON_MEMORY_REGION(result, size);
                DCHECK_LE(info.allocated_bytes + size, info.chunk.size);
                info.allocated_bytes += padding + size;
                total_allocated_bytes_ += padding + size;
                DCHECK_LE(current_chunk_idx_, chunks_.size() - 1);
                return result;
            }
        }

        // If we couldn't allocate a new chunk, return NULL. malloc() guarantees alignment
        // of alignof(std::max_align_t), so we do not need to do anything additional to
        // guarantee alignment.
        //static_assert(
        //INITIAL_CHUNK_SIZE >= config::FLAGS_MEMORY_MAX_ALIGNMENT, "Min chunk size too low");
        if (UNLIKELY(!find_chunk(size + reserve, CHECK_LIMIT_FIRST))) return nullptr;

        ChunkInfo& info = chunks_[current_chunk_idx_];
        uint8_t* result = info.chunk.data + info.allocated_bytes;
        SR_ASAN_UNPOISON_MEMORY_REGION(result, size);
        DCHECK_LE(info.allocated_bytes + size, info.chunk.size);
        info.allocated_bytes += size;
        total_allocated_bytes_ += size;
        DCHECK_LE(current_chunk_idx_, chunks_.size() - 1);
        peak_allocated_bytes_ = std::max(total_allocated_bytes_, peak_allocated_bytes_);
        return result;
    }

    /// chunk from which we served the last Allocate() call;
    /// always points to the last chunk that contains allocated data;
    /// chunks 0..current_chunk_idx_ - 1 are guaranteed to contain data
    /// (chunks_[i].allocated_bytes > 0 for i: 0..current_chunk_idx_ - 1);
    /// chunks after 'current_chunk_idx_' are "free chunks" that contain no data.
    /// -1 if no chunks present
    int current_chunk_idx_{-1};

    /// The size of the next chunk to allocate.
    int next_chunk_size_;

    /// sum of allocated_bytes_
    int64_t total_allocated_bytes_{0};

    /// sum of all bytes allocated in chunks_
    int64_t total_reserved_bytes_{0};

    /// Maximum number of bytes allocated from this pool at one time.
    int64_t peak_allocated_bytes_{0};


    std::vector<ChunkInfo> chunks_;
};
static std::atomic<size_t> memory_pool_bytes_total{};

// Stamp out templated implementations here so they're included in IR module
template uint8_t* MemPool::allocate<false>(int64_t size, int alignment, int reserve);
template uint8_t* MemPool::allocate<true>(int64_t size, int alignment, int reserve);
/*
struct MemPool {
    size_t _allocated_bytes = 0;
    static constexpr size_t BLOCK_SIZE = 8192;
    std::vector<std::vector<char> > _blocks;

    char *allocate_with_reserve(size_t size, size_t reserve) {
        if (_blocks.empty() || _blocks.back().size() + size + reserve > BLOCK_SIZE) {
            _blocks.emplace_back();
            _blocks.back().reserve(BLOCK_SIZE);
            _allocated_bytes += BLOCK_SIZE;
        }
        auto &block = _blocks.back();
        block.resize(block.size() + size + reserve);
        auto *p = block.data() + block.size() - size - reserve;
        return p;
    }

    size_t total_allocated_bytes() const { return _allocated_bytes; }
};
*/

#define MEM_POOL_POISON (0x66aa77bb)

const int MemPool::INITIAL_CHUNK_SIZE;
const int MemPool::MAX_CHUNK_SIZE;

const int MemPool::DEFAULT_ALIGNMENT;
uint32_t MemPool::k_zero_length_region_ alignas(std::max_align_t) = MEM_POOL_POISON;

MemPool::ChunkInfo::ChunkInfo(const MemChunk& chunk_) : chunk(chunk_) {
    memory_pool_bytes_total.fetch_add(chunk.size);
}

MemPool::~MemPool() {
    int64_t total_bytes_released = 0;
    for (auto& chunk : chunks_) {
        total_bytes_released += chunk.chunk.size;
        MemChunkAllocator::free(chunk.chunk);
    }
    memory_pool_bytes_total.fetch_add(-total_bytes_released);
}

void MemPool::clear() {
    current_chunk_idx_ = -1;
    for (auto& chunk : chunks_) {
        chunk.allocated_bytes = 0;
        SR_ASAN_POISON_MEMORY_REGION(chunk.chunk.data, chunk.chunk.size);
    }
    total_allocated_bytes_ = 0;
    DCHECK(check_integrity(false));
}

void MemPool::free_all() {
    int64_t total_bytes_released = 0;
    for (auto& chunk : chunks_) {
        total_bytes_released += chunk.chunk.size;
        MemChunkAllocator::free(chunk.chunk);
    }
    chunks_.clear();
    next_chunk_size_ = INITIAL_CHUNK_SIZE;
    current_chunk_idx_ = -1;
    total_allocated_bytes_ = 0;
    total_reserved_bytes_ = 0;

   memory_pool_bytes_total.fetch_add(-total_bytes_released);
}

bool MemPool::find_chunk(size_t min_size, bool check_limits) {
    // Try to allocate from a free chunk. We may have free chunks after the current chunk
    // if Clear() was called. The current chunk may be free if ReturnPartialAllocation()
    // was called. The first free chunk (if there is one) can therefore be either the
    // current chunk or the chunk immediately after the current chunk.
    int first_free_idx;
    if (current_chunk_idx_ == -1) {
        first_free_idx = 0;
    } else {
        DCHECK_GE(current_chunk_idx_, 0);
        first_free_idx = current_chunk_idx_ + (chunks_[current_chunk_idx_].allocated_bytes > 0);
    }
    for (int idx = current_chunk_idx_ + 1; idx < chunks_.size(); ++idx) {
        // All chunks after 'current_chunk_idx_' should be free.
        DCHECK_EQ(chunks_[idx].allocated_bytes, 0);
        if (chunks_[idx].chunk.size >= min_size) {
            // This chunk is big enough. Move it before the other free chunks.
            if (idx != first_free_idx) std::swap(chunks_[idx], chunks_[first_free_idx]);
            current_chunk_idx_ = first_free_idx;
            DCHECK(check_integrity(true));
            return true;
        }
    }

    // Didn't find a big enough free chunk - need to allocate new chunk.
    size_t chunk_size;
    DCHECK_LE(next_chunk_size_, MAX_CHUNK_SIZE);

        DCHECK_GE(next_chunk_size_, INITIAL_CHUNK_SIZE);
        chunk_size = std::max<size_t>(min_size, next_chunk_size_);

    chunk_size = RoundUpToPowerOf2(chunk_size, 1);

    // Allocate a new chunk. Return early if allocate fails.
    MemChunk chunk;
    if (!MemChunkAllocator::allocate(chunk_size, &chunk)) {
        throw std::bad_alloc();
    }
    SR_ASAN_POISON_MEMORY_REGION(chunk.data, chunk_size);
    // Put it before the first free chunk. If no free chunks, it goes at the end.
    if (first_free_idx == static_cast<int>(chunks_.size())) {
        chunks_.emplace_back(chunk);
    } else {
        chunks_.insert(chunks_.begin() + first_free_idx, ChunkInfo(chunk));
    }
    current_chunk_idx_ = first_free_idx;
    total_reserved_bytes_ += chunk_size;
    // Don't increment the chunk size until the allocation succeeds: if an attempted
    // large allocation fails we don't want to increase the chunk size further.
    next_chunk_size_ = static_cast<int>(std::min<int64_t>(chunk_size * 2, MAX_CHUNK_SIZE));

    DCHECK(check_integrity(true));
    return true;
}

void MemPool::acquire_data(MemPool* src, bool keep_current) {
    DCHECK(src->check_integrity(false));
    int num_acquired_chunks;
    if (keep_current) {
        num_acquired_chunks = src->current_chunk_idx_;
    } else if (src->get_free_offset() == 0) {
        // nothing in the last chunk
        num_acquired_chunks = src->current_chunk_idx_;
    } else {
        num_acquired_chunks = src->current_chunk_idx_ + 1;
    }

    if (num_acquired_chunks <= 0) {
        if (!keep_current) src->free_all();
        return;
    }

    auto end_chunk = src->chunks_.begin() + num_acquired_chunks;
    int64_t total_transfered_bytes = 0;
    for (auto i = src->chunks_.begin(); i != end_chunk; ++i) {
        total_transfered_bytes += i->chunk.size;
    }
    src->total_reserved_bytes_ -= total_transfered_bytes;
    total_reserved_bytes_ += total_transfered_bytes;

    // insert new chunks after current_chunk_idx_
    auto insert_chunk = chunks_.begin() + current_chunk_idx_ + 1;
    chunks_.insert(insert_chunk, src->chunks_.begin(), end_chunk);
    src->chunks_.erase(src->chunks_.begin(), end_chunk);
    current_chunk_idx_ += num_acquired_chunks;

    if (keep_current) {
        src->current_chunk_idx_ = 0;
        DCHECK(src->chunks_.size() == 1 || src->chunks_[1].allocated_bytes == 0);
        total_allocated_bytes_ += src->total_allocated_bytes_ - src->get_free_offset();
        src->total_allocated_bytes_ = src->get_free_offset();
    } else {
        src->current_chunk_idx_ = -1;
        total_allocated_bytes_ += src->total_allocated_bytes_;
        src->total_allocated_bytes_ = 0;
    }

    peak_allocated_bytes_ = std::max(total_allocated_bytes_, peak_allocated_bytes_);

    if (!keep_current) src->free_all();
    DCHECK(src->check_integrity(false));
    DCHECK(check_integrity(false));
}

void MemPool::exchange_data(MemPool* other) {
    std::swap(current_chunk_idx_, other->current_chunk_idx_);
    std::swap(next_chunk_size_, other->next_chunk_size_);
    std::swap(total_allocated_bytes_, other->total_allocated_bytes_);
    std::swap(total_reserved_bytes_, other->total_reserved_bytes_);
    std::swap(peak_allocated_bytes_, other->peak_allocated_bytes_);
    std::swap(chunks_, other->chunks_);
}

std::string MemPool::debug_string() {
    std::stringstream out;
    char str[16];
    out << "MemPool(#chunks=" << chunks_.size() << " [";
    for (int i = 0; i < chunks_.size(); ++i) {
        sprintf(str, "0x%lx=", reinterpret_cast<size_t>(chunks_[i].chunk.data));
        out << (i > 0 ? " " : "") << str << chunks_[i].chunk.size << "/" << chunks_[i].allocated_bytes;
    }
    out << "] current_chunk=" << current_chunk_idx_ << " total_sizes=" << total_reserved_bytes_
        << " total_alloc=" << total_allocated_bytes_ << ")";
    return out.str();
}

bool MemPool::check_integrity(bool check_current_chunk_empty) {
    DCHECK_LT(current_chunk_idx_, static_cast<int>(chunks_.size()));

    // Without pooling, there are way too many chunks and this takes too long.
    // check that current_chunk_idx_ points to the last chunk with allocated data
    int64_t total_allocated = 0;
    for (int i = 0; i < chunks_.size(); ++i) {
        DCHECK_GT(chunks_[i].chunk.size, 0);
        if (i < current_chunk_idx_) {
            DCHECK_GT(chunks_[i].allocated_bytes, 0);
        } else if (i == current_chunk_idx_) {
            DCHECK_GE(chunks_[i].allocated_bytes, 0);
            if (check_current_chunk_empty) DCHECK_EQ(chunks_[i].allocated_bytes, 0);
        } else {
            DCHECK_EQ(chunks_[i].allocated_bytes, 0);
        }
        total_allocated += chunks_[i].allocated_bytes;
    }
    DCHECK_EQ(total_allocated, total_allocated_bytes_);
    return true;
}

typedef unsigned __int128 uint128_t;

template<int n>
struct phmap_mix {
    inline size_t operator()(size_t) const;
};

template<>
class phmap_mix<4> {
public:
    inline size_t operator()(size_t a) const {
        static constexpr uint64_t kmul = 0xcc9e2d51UL;
        uint64_t l = a * kmul;
        return static_cast<size_t>(l ^ (l >> 32u));
    }
};

template<>
class phmap_mix<8> {
public:
    // Very fast mixing (similar to Abseil)
    inline size_t operator()(size_t a) const {
        static constexpr uint64_t k = 0xde5fb9d2630458e9ULL;
        uint64_t h;
        uint64_t l = umul128(a, k, &h);
        return static_cast<size_t>(h + l);
    }
};

template<class T>
class StdHash {
public:
    std::size_t operator()(T value) const { return phmap_mix<sizeof(size_t)>()(std::hash<T>()(value)); }
};

struct Slice {
    const char *data;
    size_t size;

    Slice(const char *d, size_t s) : data(d), size(s) {
    }
};


class SliceWithHash : public Slice {
public:
    size_t hash;

    SliceWithHash(const Slice &src) : Slice(src.data, src.size) {
        std::string_view sv(static_cast<const char *>(src.data), src.size);
        hash = std::hash<std::string_view>()(sv);
    }

    SliceWithHash(const char *p, size_t s, size_t h) : Slice(p, s), hash(h) {
    }
};

class HashOnSliceWithHash {
public:
    std::size_t operator()(const SliceWithHash &slice) const { return slice.hash; }
};

class EqualOnSliceWithHash {
public:
    bool operator()(const SliceWithHash &x, const SliceWithHash &y) const {
        // by comparing hash value first, we can avoid comparing real data
        // which may touch another memory area and has bad cache locality.
        return x.hash == y.hash && x.size == y.size && memcmp(x.data, y.data, x.size) == 0;
    }
};

template<typename T>
using HashSet =
phmap::flat_hash_set<T, StdHash<T>, phmap::priv::hash_default_eq<T> >;

using SliceHashSet = phmap::flat_hash_set<SliceWithHash, HashOnSliceWithHash, EqualOnSliceWithHash>;
using SliceTwoLevelHashSet =
phmap::parallel_flat_hash_set<SliceWithHash, HashOnSliceWithHash, EqualOnSliceWithHash>;

struct AdaptiveSliceHashSet {
    using KeyType = typename SliceHashSet::key_type;

    AdaptiveSliceHashSet() { set = std::make_shared<SliceHashSet>(); }

    void clear() {
        set = std::make_shared<SliceHashSet>();
        two_level_set.reset();
        distinct_size = 0;
    }

    void try_convert_to_two_level(MemPool *mem_pool) {
        if (distinct_size % 65536 == 0 && mem_pool->total_allocated_bytes() >= 32*1024*1024) {
            two_level_set = std::make_shared<SliceTwoLevelHashSet>();
            two_level_set->reserve(set->capacity());
            two_level_set->insert(set->begin(), set->end());
            set.reset();
        }
    }

    void emplace(MemPool *mem_pool, Slice raw_key) {
        KeyType key(raw_key);
        if (set != nullptr) {
#if defined(__clang__) && (__clang_major__ >= 32*1024*1024)
            set->lazy_emplace(key, [&](const auto &ctor) {
#else
            set->lazy_emplace(key, [&](const auto &ctor) {
#endif
                auto *pos = mem_pool->allocate_with_reserve(key.size, 15);
                assert(pos != nullptr);
                memcpy(pos, key.data, key.size);
                ctor(reinterpret_cast<char*>(pos), key.size, key.hash);
                distinct_size++;
                try_convert_to_two_level(mem_pool);
            });
        } else {
#if defined(__clang__) && (__clang_major__ >= 16)
            two_level_set->lazy_emplace(key, [&](const auto &ctor) {
#else
            two_level_set->lazy_emplace(key, [&](const auto &ctor) {
#endif
                auto *pos = mem_pool->allocate_with_reserve(key.size, 15);
                assert(pos != nullptr);
                memcpy(pos, key.data, key.size);
                ctor(reinterpret_cast<char*>(pos), key.size, key.hash);
                distinct_size++;
            });
        }
    }

    void lazy_emplace_with_hash(MemPool *mem_pool, Slice raw_key, size_t hash) {
        KeyType key(raw_key.data, raw_key.size, hash);
        if (set != nullptr) {
#if defined(__clang__) && (__clang_major__ >= 16)
            set->lazy_emplace_with_hash(key, hash, [&](const auto &ctor) {
#else
            set->lazy_emplace_with_hash(key, hash, [&](const auto &ctor) {
#endif
                auto *pos = mem_pool->allocate_with_reserve(key.size, 15);
                assert(pos != nullptr);
                memcpy(pos, key.data, key.size);
                ctor(reinterpret_cast<char*>(pos), key.size, key.hash);
                distinct_size++;
                try_convert_to_two_level(mem_pool);
            });
        } else {
#if defined(__clang__) && (__clang_major__ >= 16)
            two_level_set->lazy_emplace_with_hash(key, hash, [&](const auto &ctor) {
#else
            two_level_set->lazy_emplace_with_hash(key, hash, [&](const auto &ctor) {
#endif
                auto *pos = mem_pool->allocate_with_reserve(key.size, 15);
                assert(pos != nullptr);
                memcpy(pos, key.data, key.size);
                ctor(reinterpret_cast<char*>(pos), key.size, key.hash);
            });
        }
    }

    void prefetch_hash(size_t hash_value) {
        if (set != nullptr) {
            set->prefetch_hash(hash_value);
        } else {
            two_level_set->prefetch_hash(hash_value);
        }
    }

    int64_t serialize_size() const {
        size_t size = 0;
        if (set != nullptr) {
            for (auto &key: *set) {
                size += key.size + sizeof(uint32_t);
            }
        } else {
            for (auto &key: *two_level_set) {
                size += key.size + sizeof(uint32_t);
            }
        }
        return size;
    }

    void serialize(uint8_t *dst) const {
        if (set != nullptr) {
            for (auto &key: *set) {
                auto size = (uint32_t) key.size;
                memcpy(dst, &size, sizeof(uint32_t));
                dst += sizeof(uint32_t);
                memcpy(dst, key.data, key.size);
                dst += key.size;
            }
        } else {
            for (auto &key: *two_level_set) {
                auto size = (uint32_t) key.size;
                memcpy(dst, &size, sizeof(uint32_t));
                dst += sizeof(uint32_t);
                memcpy(dst, key.data, key.size);
                dst += key.size;
            }
        }
    }

    void fill_vector(std::vector<std::string> &values) const {
        if (set != nullptr) {
            for (const auto &v: *set) {
                values.emplace_back(v.data, v.size);
            }
        } else {
            for (const auto &v: *two_level_set) {
                values.emplace_back(v.data, v.size);
            }
        }
    }

    int64_t size() const { return distinct_size; }

    std::shared_ptr<SliceHashSet> set;
    std::shared_ptr<SliceTwoLevelHashSet> two_level_set;
    int64_t distinct_size = 0;

    HashOnSliceWithHash hash_function() const { return HashOnSliceWithHash(); }
};

TEST(TestPhmap, test_parallel_flat_map) {
    AdaptiveSliceHashSet set;
    std::unordered_set<std::string> set2;
    AdaptiveSliceHashSet set3;
    MemPool mem_pool;
    for (size_t i=0;i < 1000000;i++) {
        struct CacheEntry {
            size_t hash_value;
        };
        std::vector<CacheEntry> cache(4096);
        std::vector<std::string> data;
        data.reserve(4096);
        for (size_t n = 0; n < 4096;++n) {
            std::string s(4096, 'x');
            Slice slice(s.data(), s.size());
            auto m = i*4096 + n;
            std::string s2 = std::to_string(m);
            auto* p = s.data()+s.size()-s2.size();
            memcpy(p, s2.data(),s2.size());
            data.push_back(s);
            size_t hash_value = std::hash<std::string_view>()(std::string_view(s.data(), s.size()));
            cache[i] = CacheEntry{hash_value};
        }
        // This is just an empirical value based on benchmark, and you can tweak it if more proper value is found.
        size_t prefetch_index = 16;

        for (size_t n = 0; n < 4096; ++n) {
            if (prefetch_index < 4096) {
                set.prefetch_hash(cache[prefetch_index].hash_value);
                prefetch_index++;
            }
            auto &d = data[n];
            set.lazy_emplace_with_hash(&mem_pool, Slice(d.data(),d.size()), cache[i].hash_value);
            set2.insert(d);
            set3.emplace(&mem_pool, Slice(d.data(),d.size()));
            // set.emplace(&mem_pool, Slice(d.data(),d.size()));
            //std::cout<<"d="<<d<<",dist_size="<<set.distinct_size<<",size="<<set.size()<<std::endl;
        }
        if (((i+1)*4096)%(4096*64)==0) {
            std::cout << i *4096<<": set.distinct_size="<<set.distinct_size <<", set.size="<<set.size()
            <<", set2.size= "<<set2.size()
            <<", set3.distinct_size"<< set3.distinct_size<<"set3.size="<<set3.size()<<std::endl;
        }
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
