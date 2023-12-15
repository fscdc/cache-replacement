#include <algorithm>
#include <cassert>
#include <map>
#include <vector>

#include "cache.h"

namespace
{
    std::map<CACHE*, std::vector<uint64_t>> last_used_cycles;
    std::map<CACHE*, std::vector<uint8_t>> segment_tracker; // 新增，用于跟踪每个块的分区
    constexpr uint8_t NUM_SEGMENTS = 4; // 定义分区的数量
}

void CACHE::initialize_replacement() 
{
    ::last_used_cycles[this] = std::vector<uint64_t>(NUM_SET * NUM_WAY);
    ::segment_tracker[this] = std::vector<uint8_t>(NUM_SET * NUM_WAY, 0); // 初始化所有块到最低级分区
}

uint32_t CACHE::find_victim(uint32_t triggering_cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    auto& segments = ::segment_tracker[this];
    auto begin = std::next(std::begin(::last_used_cycles[this]), set * NUM_WAY);
    auto end = std::next(begin, NUM_WAY);

    // 找到属于最低级分区的块中，最久未使用的块
    auto victim_it = std::find_if(begin, end, [&](const uint64_t& cycle) {
        auto it = std::find(begin, end, cycle);
        uint32_t way = static_cast<uint32_t>(std::distance(begin, it));
        return segments[set * NUM_WAY + way] == 0;
    });

    assert(victim_it != end); // 确保总能找到一个victim
    return static_cast<uint32_t>(std::distance(begin, victim_it));
}





void CACHE::update_replacement_state(uint32_t triggering_cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    auto& segments = ::segment_tracker[this];
    uint32_t index = set * NUM_WAY + way;

    // 更新访问周期
    ::last_used_cycles[this][index] = current_cycle;

    // 根据访问更新块的分区
    if (hit) {
        segments[index] = std::min(segments[index] + 1, NUM_SEGMENTS - 1);
    } else {
        // 对于misses，新装入的块放入最低级分区
        segments[index] = 0;
    }
}

void CACHE::replacement_final_stats() {}
