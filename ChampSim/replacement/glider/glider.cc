#include <map>
#include <math.h>

#include "cache.h"

// 定义了系统中的核心数量，这里设置为1
#define NUM_CORE 1
// 定义了每个核心的最后一级cache(LLC)集合的数量，这里是每个核心2048个集合
#define LLC_SETS NUM_CORE * 2048
// 定义了LLC每个集合中的路数（ways），这里设置为16路
#define LLC_WAYS 16

// RRIP计数器
#define MAXRRIP 7
#define MIDRRIP 2
// 定义了一个二维数组来存储LLC中每个集合的每条路的RRIP值
uint32_t rrip[LLC_SETS][LLC_WAYS];

#include <iostream>

#include "glider_predictor.h"
#include "helper_function.h"
#include "optgen.h"

// Glider预测器
Glider_Predictor *predictor_demand;

// OPTgen
OPTgen optgen_occup_vector[LLC_SETS];

// 采样器组件跟踪cache历史记录
#define SAMPLER_ENTRIES 2800
#define SAMPLER_HIST 8
// 计算采样器集合的数量，即2800个entry给每个集合分8个entry用作历史记录
#define SAMPLER_SETS SAMPLER_ENTRIES / SAMPLER_HIST
// 采样器的cache历史记录，map里的uint64_t是访存地址对应的tag
vector<map<uint64_t, HISTORY>> cache_history_sampler;
// 每个集合的每条路的样本签名，用于跟踪哪些指令最近使用了cache行
uint64_t sample_signature[LLC_SETS][LLC_WAYS];

// 历史时间
#define TIMER_SIZE 1024
// 每个采样器集合的时间戳计数器，用于OPTgen算法中跟踪cache行的年龄
uint64_t set_timer[LLC_SETS];

// 数学函数用于计算采样集合
#define bitmask(l) (((l) == 64) ? (unsigned long long)(-1LL) : ((1LL << (l)) - 1LL))
// 从x中提取从i开始的l长度的位
#define bits(x, i, l) (((x) >> (i)) & bitmask(l))
// 辅助函数用于采样每个核心的64个集合，利用位操作来决定一个集合是否被采样
#define SAMPLED_SET(set) (bits(set, 0, 6) == bits(set, ((unsigned long long)log2(LLC_SETS) - 6), 6))

/**
 * 初始化Glider替换策略状态。
 */
void CACHE::initialize_replacement()
{
    cout << "Initialize Glider replacement policy state" << endl;

    for (int i = 0; i < LLC_SETS; i++)
    {
        for (int j = 0; j < LLC_WAYS; j++)
        {
            rrip[i][j] = MAXRRIP;
            sample_signature[i][j] = 0;
        }
        // 先全初始化为0，对每个样本，时间戳当前都是0
        set_timer[i] = 0;
        optgen_occup_vector[i].init(LLC_WAYS - 2);
    }

    cache_history_sampler.resize(SAMPLER_SETS);
    for (int i = 0; i < SAMPLER_SETS; i++)
    {
        cache_history_sampler[i].clear();
    }

    predictor_demand = new Glider_Predictor();

    cout << "Finished initializing Glider replacement policy state" << endl;
}

/**
 * 根据替换策略找到一个受害cache行。
 *
 * @param triggering_cpu 触发替换的CPU的ID。
 * @param instr_id 触发替换的指令的ID。
 * @param set cache集合的索引。
 * @param current_set 当前cache集合的指针。
 * @param ip 指令指针。
 * @param full_addr 内存访问的完整地址。
 * @param type 内存访问的类型。
 * @return 受害cache行的索引。
 */
uint32_t CACHE::find_victim(uint32_t triggering_cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
    // 根据RRPV值为7（低优先级）来找到一个受害者
    for (uint32_t i = 0; i < LLC_WAYS; i++)
    {
        if (rrip[set][i] == MAXRRIP)
        {
            return i;
        }
    }

    // 根据RRPV值为2（中等优先级）来找到一个受害者
    for (uint32_t i = 0; i < LLC_WAYS; i++)
    {
        if (rrip[set][i] == MIDRRIP)
        {
            return i;
        }
    }

    // 如果没有RRPV值为7或2的cache行，那么我们找到下一个最高的RRPV值（最老的cache友好行）
    uint32_t max_rrpv = 0;
    int32_t victim = -1;
    for (uint32_t i = 0; i < LLC_WAYS; i++)
    {
        if (rrip[set][i] >= max_rrpv)
        {
            max_rrpv = rrip[set][i];
            victim = i;
        }
    }

    // 训练predictor
    if (SAMPLED_SET(set))
    {
        cout << "Decrease" << endl;
        predictor_demand->decrease(sample_signature[set][victim]);

        // 输出调试信息
        predictor_demand->print_all_weights();
    }

    return victim;
}

/**
 * 为给定的样本集更新cache历史记录的当前值。
 * 此函数会为所有LRU值小于currentVal的cache历史记录条目自增LRU。
 *
 * @param sample_set 要更新cache历史记录的样本集。
 * @param currentVal 用于与LRU值比较的当前值。
 */
void update_cache_history(unsigned int sample_set, unsigned int currentVal)
{
    // 遍历给定采样集合中的cache历史记录
    for (map<uint64_t, HISTORY>::iterator it = cache_history_sampler[sample_set].begin(); it != cache_history_sampler[sample_set].end(); it++)
    {
        // 如果当前条目的LRU值小于currentVal，则将其LRU值增加1
        if ((it->second).lru < currentVal)
        {
            (it->second).lru++;
        }
    }
}

/**
 * 更新给定组和路的cache替换状态。
 *
 * @param triggering_cpu 触发更新的CPU的ID。
 * @param set cache的组索引。
 * @param way cache的路索引。
 * @param full_addr 被访问的内存的完整地址。
 * @param ip 访问指令的指令指针。
 * @param victim_addr 受害者行的地址。
 * @param type 访问操作的类型。
 * @param hit 表示访问是否导致cache命中。
 */
void CACHE::update_replacement_state(uint32_t triggering_cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type,
                                     uint8_t hit)
{
    // 将地址对齐到cache行边界
    full_addr = (full_addr >> 6) << 6;

    // 如果当前的cache集合(set)被选中用于进行OPT替换算法的取样
    if (SAMPLED_SET(set))
    {
        // 获取当前cache集合的时间戳，并将其模上OPTGEN_SIZE，以获取当前的时间戳在OPTGEN范围内的值
        uint64_t currentVal = set_timer[set] % OPTGEN_SIZE;
        // 对完整地址进行CRC散列，然后模上256，得到取样标签
        uint64_t sample_tag = CRC(full_addr >> 12) % 256;
        // 通过右移6位（忽略掉cache行信息），然后对SAMPLER_SETS取模来得到取样集合
        uint32_t sample_set = (full_addr >> 6) % SAMPLER_SETS;

        // 如果cache历史记录中存在此标签（即这不是首次访问），且操作是需求访问（非预取）
        if (cache_history_sampler[sample_set].find(sample_tag) != cache_history_sampler[sample_set].end())
        {
            // 获取当前时间
            unsigned int current_time = set_timer[set];
            // 如果当前时间小于之前记录的时间，则将当前时间增加TIMER_SIZE，这可能是因为发生了时间环绕
            if (current_time < cache_history_sampler[sample_set][sample_tag].previousVal)
            {
                current_time += TIMER_SIZE;
            }
            // 获取之前的时间值，并进行模OPTGEN_SIZE操作
            uint64_t previousVal = cache_history_sampler[sample_set][sample_tag].previousVal % OPTGEN_SIZE;
            // 判断是否发生了时间环绕
            bool isWrap = (current_time - cache_history_sampler[sample_set][sample_tag].previousVal) > OPTGEN_SIZE;

            // 如果没有时间环绕，并且OPTgen表示PC值被cache，则提高该PC值的预测值
            if (!isWrap && optgen_occup_vector[set].is_cache(currentVal, previousVal))
            {
                cout << "Increase" << endl;
                predictor_demand->increase(cache_history_sampler[sample_set][sample_tag].PCval);
            }
            // 如果OPT预测器表示该行没有被cache，则降低该PC值的预测值
            else
            {
                cout << "Decrease" << endl;
                predictor_demand->decrease(cache_history_sampler[sample_set][sample_tag].PCval);
            }

            // 输出调试信息
            predictor_demand->print_all_weights();

            // 设置当前值为已访问状态
            optgen_occup_vector[set].set_access(currentVal);
            // 更新cache历史记录
            update_cache_history(sample_set, cache_history_sampler[sample_set][sample_tag].lru);
        }
        // 如果此行之前未被使用过，则将其标记为需求访问
        else
        {
            // 如果正在进行取样，并且cache历史记录的大小已达上限，则需要找到一个替代者
            if (cache_history_sampler[sample_set].size() == SAMPLER_HIST)
            {
                // 从cache历史记录中找到最久未使用的元素并将其替换
                uint64_t addr_val = 0;
                for (map<uint64_t, HISTORY>::iterator it = cache_history_sampler[sample_set].begin(); it != cache_history_sampler[sample_set].end(); it++)
                {
                    // 其实就是找lru是SAMPLER_HIST - 1的元素
                    if ((it->second).lru == (SAMPLER_HIST - 1))
                    {
                        addr_val = it->first;
                        break;
                    }
                }
                cache_history_sampler[sample_set].erase(addr_val);
            }

            // 为新行创建一个条目
            cache_history_sampler[sample_set][sample_tag].init();
            // 设置为需求访问
            optgen_occup_vector[set].set_access(currentVal);

            // 更新cache历史记录，如果entry的LRU值小于SAMPLER_HIST - 1，则将其LRU值增加1
            // 这也保证了，如果cache历史记录的大小已达上限，能够找到lru是SAMPLER_HIST - 1的替换者
            update_cache_history(sample_set, SAMPLER_HIST - 1);
        }
        // 更新取样的时间和PC
        cache_history_sampler[sample_set][sample_tag].update(set_timer[set], ip);
        // 将最近使用的条目的LRU值设为0
        cache_history_sampler[sample_set][sample_tag].lru = 0;
        // 更新集合的时间戳，又过了一个时间点了
        set_timer[set] = (set_timer[set] + 1) % TIMER_SIZE;
    }

    // 从Glider预测器获取对此行的预测结果
    Prediction prediction = predictor_demand->get_prediction(ip);

    // 记录此cache行的最后一个IP值
    sample_signature[set][way] = ip;
    // 如果Glider决定以低优先级插入，则将其RRIP值设为MAXRRIP
    if (prediction == Prediction::Low)
    {
        rrip[set][way] = MAXRRIP;
    }
    // 如果Glider决定以中等优先级插入，则将其RRIP值设为MIDRRIP
    else if (prediction == Prediction::Medium)
    {
        rrip[set][way] = MIDRRIP;
    }
    // 如果Glider决定以高优先级插入
    else
    {
        rrip[set][way] = 0;
        // 如果这次访问是一个未命中，则检查所有行的RRIP值是否已饱和
        if (!hit)
        {
            // 验证RRPV是否已经达到最大值
            bool isMaxVal = false;
            for (uint32_t i = 0; i < LLC_WAYS; i++)
            {
                if (rrip[set][i] == MIDRRIP - 1)
                {
                    isMaxVal = true;
                }
            }

            // 如果没有达到最大值，则使所有友好cache行老化（增加其RRIP值）
            for (uint32_t i = 0; i < LLC_WAYS; i++)
            {
                if (!isMaxVal && rrip[set][i] < MIDRRIP - 1)
                {
                    rrip[set][i]++;
                }
            }
        }
        // 将当前行的RRIP值设为0，表示最近使用过
        rrip[set][way] = 0;
    }
}

void CACHE::replacement_final_stats() {}
