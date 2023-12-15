#ifndef OPTGEN_H
#define OPTGEN_H

using namespace std;

#include <stdint.h>
#include <vector>
#define OPTGEN_SIZE 128

struct OPTgen
{
    vector<unsigned int> liveness_intervals;
    uint64_t num_cache;
    uint64_t access;
    uint64_t cache_size;

    // 初始化
    void init(uint64_t size)
    {
        // cache hit次数
        num_cache = 0;
        // 访问次数
        access = 0;
        // cache大小
        cache_size = size;
        // liveness_intervals初始化为0
        liveness_intervals.resize(OPTGEN_SIZE, 0);
    }

    // 返回hit次数
    uint64_t get_optgen_hits() { return num_cache; }

    void set_access(uint64_t val)
    {
        access++;
        liveness_intervals[val] = 0;
    }

    // 返回hit还是miss（Belady的高效实现，参考Hawkeye）
    bool is_cache(uint64_t val, uint64_t endVal)
    {
        bool cache = true;
        unsigned int count = endVal;

        // endVal ~ val之间如果有一个值的liveness_intervals大于cache_size，那么就不cache
        while (count != val)
        {
            if (liveness_intervals[count] >= cache_size)
            {
                cache = false;
                break;
            }
            count = (count + 1) % liveness_intervals.size();
        }

        // 如果选择cache，那么将endVal ~ val之间的liveness_intervals都加1
        if (cache)
        {
            count = endVal;
            while (count != val)
            {
                liveness_intervals[count]++;
                count = (count + 1) % liveness_intervals.size();
            }
            num_cache++;
        }
        return cache;
    }
};

#endif