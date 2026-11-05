#pragma once

// 캐시 예산. 여러 캐시의 메모리를 한곳에서 세고, 넘치면 덜 중요한 등급부터 비운다.
// 셸은 플랫폼 신호(메모리 경고, 창 숨김 등)를 trimCaches 로 넘기기만 한다.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace chwp::render {

// 작을수록 오래 남긴다. Visible 은 이번 프레임에 쓴 것이라 버리지 않는다.
enum class CacheTier : int { Visible = 0, Near = 1, Recent = 2, Spare = 3 };

// 셸이 넘기는 메모리 신호의 세기.
// Light 는 Spare, Moderate 는 Recent 까지, Critical 은 Near 까지 비운다.
enum class TrimLevel : int { Light = 1, Moderate = 2, Critical = 3 };

inline constexpr std::size_t kNoCacheBudget = std::numeric_limits<std::size_t>::max();

// 예산 관리자에 올리는 캐시 하나.
class CachePool {
public:
    virtual ~CachePool() = default;
    virtual const char* name() const = 0;
    virtual std::size_t bytes() const = 0;
    // tier 등급 항목을 오래 안 쓴 것부터 need 바이트까지 버린다. 줄인 바이트를 돌려준다.
    // 버리지 못하는 캐시는 0 을 돌려준다.
    virtual std::size_t release(CacheTier tier, std::size_t need) = 0;
};

struct CacheUsage {
    std::string name;
    std::size_t bytes = 0;
    std::size_t budget = kNoCacheBudget;
};

// 화면 루프가 프레임마다 한 번 부른다. 이 번호로 쓴 항목이 그 프레임의 Visible 이다.
void beginCacheFrame();
std::uint64_t cacheFrame();
inline CacheTier tierForFrame(std::uint64_t lastFrame) {
    return lastFrame == cacheFrame() ? CacheTier::Visible : CacheTier::Recent;
}

void registerCachePool(CachePool& pool, std::size_t budget);
void unregisterCachePool(CachePool& pool);
// 이름이 같은 캐시의 예산을 바꾼다. 아직 등록하지 않은 이름이면 기억해 두었다가 등록할 때 쓰고 거짓을 돌려준다.
bool setCacheBudget(const std::string& name, std::size_t budget);

// pool 에 걸린 예산. 없으면 kNoCacheBudget.
std::size_t cacheBudget(const CachePool& pool);
// pool 이 자기 예산을 넘었으면 Spare, Recent, Near 순으로 비운다. Visible 은 남긴다.
void enforceCacheBudget(CachePool& pool);
void trimCaches(TrimLevel level);
std::vector<CacheUsage> cacheUsage();

}  // namespace chwp::render
