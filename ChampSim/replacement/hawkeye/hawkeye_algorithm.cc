#include <map>
#include <math.h>

#include "cache.h"

#define NUM_CORE 1
#define LLC_SETS NUM_CORE * 2048
#define LLC_WAYS 16

// 3-bit RRIP counter
#define MAXRRIP 7
uint32_t rrip[LLC_SETS][LLC_WAYS];

#include <iostream>

#include "hawkeye_predictor.h"
#include "helper_function.h"
#include "optgen.h"

// Hawkeye predictors for demand requests
Hawkeye_Predictor* predictor_demand; // 2K entries, 5-bit counter per each entry

OPTgen optgen_occup_vector[LLC_SETS]; // 64 vecotrs, 128 entries each

// Sampler components tracking cache history
#define SAMPLER_ENTRIES 2800
#define SAMPLER_HIST 8
#define SAMPLER_SETS SAMPLER_ENTRIES / SAMPLER_HIST
vector<map<uint64_t, HISTORY>> cache_history_sampler; // 2800 entries, 4-bytes per each entry
uint64_t sample_signature[LLC_SETS][LLC_WAYS];

// History time
#define TIMER_SIZE 1024
uint64_t set_timer[LLC_SETS]; // 64 sets, where 1 timer is used for every set

// Mathmatical functions needed for sampling set
#define bitmask(l) (((l) == 64) ? (unsigned long long)(-1LL) : ((1LL << (l)) - 1LL))
#define bits(x, i, l) (((x) >> (i)) & bitmask(l))
#define SAMPLED_SET(set) (bits(set, 0, 6) == bits(set, ((unsigned long long)log2(LLC_SETS) - 6), 6)) // Helper function to sample 64 sets for each core

// Initialize replacement state
void CACHE::initialize_replacement()
{
  cout << "Initialize Hawkeye replacement policy state" << endl;

  for (int i = 0; i < LLC_SETS; i++) {
    for (int j = 0; j < LLC_WAYS; j++) {
      rrip[i][j] = MAXRRIP;
      sample_signature[i][j] = 0;
    }
    set_timer[i] = 0;
    optgen_occup_vector[i].init(LLC_WAYS - 2);
  }

  cache_history_sampler.resize(SAMPLER_SETS);
  for (int i = 0; i < SAMPLER_SETS; i++) {
    cache_history_sampler[i].clear();
  }

  predictor_demand = new Hawkeye_Predictor();

  cout << "Finished initializing Hawkeye replacement policy state" << endl;
}

// Find replacement victim
// Return value should be 0 ~ 15 or 16 (bypass)
uint32_t CACHE::find_victim(uint32_t triggering_cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t ip, uint64_t full_addr, uint32_t type)
{
  // Find the line with RRPV of 7 in that set
  for (uint32_t i = 0; i < LLC_WAYS; i++) {
    if (rrip[set][i] == MAXRRIP) {
      return i;
    }
  }

  // If no RRPV of 7, then we find next highest RRPV value (oldest cache-friendly line)
  uint32_t max_rrpv = 0;
  int32_t victim = -1;
  for (uint32_t i = 0; i < LLC_WAYS; i++) {
    if (rrip[set][i] >= max_rrpv) {
      max_rrpv = rrip[set][i];
      victim = i;
    }
  }

  // Asserting that LRU victim is not -1
  // Predictor will be trained negaively on evictions
  if (SAMPLED_SET(set)) {
    predictor_demand->decrease(sample_signature[set][victim]);
  }

  return victim;
}

// Helper function for "UpdateReplacementState" to update cache history
void update_cache_history(unsigned int sample_set, unsigned int currentVal)
{
  for (map<uint64_t, HISTORY>::iterator it = cache_history_sampler[sample_set].begin(); it != cache_history_sampler[sample_set].end(); it++) {
    if ((it->second).lru < currentVal) {
      (it->second).lru++;
    }
  }
}

// Called on every cache hit and cache fill
void CACHE::update_replacement_state(uint32_t triggering_cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type,
                                     uint8_t hit)
{
  full_addr = (full_addr >> 6) << 6;

  // Only if we are using sampling sets for OPTgen
  if (SAMPLED_SET(set)) {
    uint64_t currentVal = set_timer[set] % OPTGEN_SIZE;
    uint64_t sample_tag = CRC(full_addr >> 12) % 256;
    uint32_t sample_set = (full_addr >> 6) % SAMPLER_SETS;

    // If line has been used before, ignoring prefetching (demand access operation)
    if (cache_history_sampler[sample_set].find(sample_tag) != cache_history_sampler[sample_set].end()) {
      unsigned int current_time = set_timer[set];
      if (current_time < cache_history_sampler[sample_set][sample_tag].previousVal) {
        current_time += TIMER_SIZE;
      }
      uint64_t previousVal = cache_history_sampler[sample_set][sample_tag].previousVal % OPTGEN_SIZE;
      bool isWrap = (current_time - cache_history_sampler[sample_set][sample_tag].previousVal) > OPTGEN_SIZE;

      // Train predictor positvely for last PC value that was prefetched
      if (!isWrap && optgen_occup_vector[set].is_cache(currentVal, previousVal)) {
        predictor_demand->increase(cache_history_sampler[sample_set][sample_tag].PCval);
      }
      // Train predictor negatively since OPT did not cache this line
      else {
        predictor_demand->decrease(cache_history_sampler[sample_set][sample_tag].PCval);
      }

      optgen_occup_vector[set].set_access(currentVal);
      // Update cache history
      update_cache_history(sample_set, cache_history_sampler[sample_set][sample_tag].lru);
    }
    // If line has not been used before, mark as demand
    else if (cache_history_sampler[sample_set].find(sample_tag) == cache_history_sampler[sample_set].end()) {
      // If sampling, find victim from cache
      if (cache_history_sampler[sample_set].size() == SAMPLER_HIST) {
        // Replace the element in the cache history
        uint64_t addr_val = 0;
        for (map<uint64_t, HISTORY>::iterator it = cache_history_sampler[sample_set].begin(); it != cache_history_sampler[sample_set].end(); it++) {
          if ((it->second).lru == (SAMPLER_HIST - 1)) {
            addr_val = it->first;
            break;
          }
        }
        cache_history_sampler[sample_set].erase(addr_val);
      }

      // Create new entry
      cache_history_sampler[sample_set][sample_tag].init();
      // set the demand access
      optgen_occup_vector[set].set_access(currentVal);

      // Update cache history
      update_cache_history(sample_set, SAMPLER_HIST - 1);
    }
    // Update the sample with time and PC
    cache_history_sampler[sample_set][sample_tag].update(set_timer[set], ip);
    cache_history_sampler[sample_set][sample_tag].lru = 0;
    set_timer[set] = (set_timer[set] + 1) % TIMER_SIZE;
  }

  // Retrieve Hawkeye's prediction for line
  bool prediction = predictor_demand->get_prediction(ip);

  sample_signature[set][way] = ip;
  // Fix RRIP counters with correct RRPVs and age accordingly
  if (!prediction) {
    rrip[set][way] = MAXRRIP;
  } else {
    rrip[set][way] = 0;
    if (!hit) {
      // Verifying RRPV of lines has not saturated
      bool isMaxVal = false;
      for (uint32_t i = 0; i < LLC_WAYS; i++) {
        if (rrip[set][i] == MAXRRIP - 1) {
          isMaxVal = true;
        }
      }

      // Aging cache-friendly lines that have not saturated
      for (uint32_t i = 0; i < LLC_WAYS; i++) {
        if (!isMaxVal && rrip[set][i] < MAXRRIP - 1) {
          rrip[set][i]++;
        }
      }
    }
    rrip[set][way] = 0;
  }
}

void CACHE::replacement_final_stats() {}

// Use this function to print out your own stats on every heartbeat
void PrintStats_Heartbeat()
{
  int hits = 0;
  int access = 0;
  for (int i = 0; i < LLC_SETS; i++) {
    hits += optgen_occup_vector[i].get_optgen_hits();
    access += optgen_occup_vector[i].access;
  }

  cout << "OPTGen Hits: " << hits << endl;
  cout << "OPTGen Access: " << access << endl;
  cout << "OPTGEN Hit Rate: " << 100 * ((double)hits / (double)access) << endl;
}

// Use this function to print out your own stats at the end of simulation
void PrintStats()
{
  int hits = 0;
  int access = 0;
  for (int i = 0; i < LLC_SETS; i++) {
    hits += optgen_occup_vector[i].get_optgen_hits();
    access += optgen_occup_vector[i].access;
  }

  cout << "Final OPTGen Hits: " << hits << endl;
  cout << "Final OPTGen Access: " << access << endl;
  cout << "Final OPTGEN Hit Rate: " << 100 * ((double)hits / (double)access) << endl;
}