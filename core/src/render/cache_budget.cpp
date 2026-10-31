#include "noting/render/cache_budget.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace noting::render {
namespace {

struct Entry {
    CachePool* pool = nullptr;
    std::size_t budget = kNoCacheBudget;
};

std::mutex& registryMutex() {
    static std::mutex mu;
    return mu;
}

std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}

// 아직 등록하지 않은 캐시에 미리 정한 예산. 처음 쓸 때 등록하는 캐시도 셸이 먼저 예산을 줄 수 있다.
std::vector<std::pair<std::string, std::size_t>>& pendingBudgets() {
    static std::vector<std::pair<std::string, std::size_t>> pending;
    return pending;
}

std::atomic<std::uint64_t>& frameCounter() {
    static std::atomic<std::uint64_t> frame{1};
    return frame;
}

// 캐시가 자기 잠금을 쥔 채 부를 수 있으니 등록표 잠금은 release 밖에서 푼다.
std::size_t budgetOf(const CachePool& pool) {
    std::lock_guard lock(registryMutex());
    for (const Entry& entry : registry()) {
        if (entry.pool == &pool) return entry.budget;
    }
    return kNoCacheBudget;
}

void releaseDownTo(CachePool& pool, CacheTier lowest, std::size_t target) {
    for (int tier = static_cast<int>(CacheTier::Spare); tier >= static_cast<int>(lowest); --tier) {
        const std::size_t used = pool.bytes();
        if (used <= target) return;
        pool.release(static_cast<CacheTier>(tier), used - target);
    }
}

}  // namespace

void beginCacheFrame() { frameCounter().fetch_add(1, std::memory_order_relaxed); }

std::uint64_t cacheFrame() { return frameCounter().load(std::memory_order_relaxed); }

void registerCachePool(CachePool& pool, std::size_t budget) {
    std::lock_guard lock(registryMutex());
    for (Entry& entry : registry()) {
        if (entry.pool == &pool) {
            entry.budget = budget;
            return;
        }
    }
    for (const auto& [name, preset] : pendingBudgets()) {
        if (name == pool.name()) {
            budget = preset;
            break;
        }
    }
    registry().push_back(Entry{&pool, budget});
}

void unregisterCachePool(CachePool& pool) {
    std::lock_guard lock(registryMutex());
    auto& entries = registry();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.pool == &pool; }),
                  entries.end());
}

bool setCacheBudget(const std::string& name, std::size_t budget) {
    CachePool* found = nullptr;
    {
        std::lock_guard lock(registryMutex());
        auto& pending = pendingBudgets();
        const auto kept = std::find_if(pending.begin(), pending.end(), [&](const auto& item) { return item.first == name; });
        if (kept != pending.end()) kept->second = budget;
        else pending.emplace_back(name, budget);
        for (Entry& entry : registry()) {
            if (name == entry.pool->name()) {
                entry.budget = budget;
                found = entry.pool;
                break;
            }
        }
    }
    if (found) enforceCacheBudget(*found);
    return found != nullptr;
}

void enforceCacheBudget(CachePool& pool) {
    const std::size_t budget = budgetOf(pool);
    if (budget == kNoCacheBudget) return;
    releaseDownTo(pool, CacheTier::Near, budget);
}

void trimCaches(TrimLevel level) {
    std::vector<CachePool*> pools;
    {
        std::lock_guard lock(registryMutex());
        for (const Entry& entry : registry()) pools.push_back(entry.pool);
    }
    const CacheTier lowest = level == TrimLevel::Critical   ? CacheTier::Near
                             : level == TrimLevel::Moderate ? CacheTier::Recent
                                                            : CacheTier::Spare;
    for (CachePool* pool : pools) releaseDownTo(*pool, lowest, 0);
}

std::vector<CacheUsage> cacheUsage() {
    std::vector<Entry> entries;
    {
        std::lock_guard lock(registryMutex());
        entries = registry();
    }
    std::vector<CacheUsage> out;
    out.reserve(entries.size());
    for (const Entry& entry : entries) {
        out.push_back(CacheUsage{entry.pool->name(), entry.pool->bytes(), entry.budget});
    }
    return out;
}

}  // namespace noting::render
