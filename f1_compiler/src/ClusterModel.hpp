#pragma once

#include "F1Model.hpp"
#include "MemoryAllocator.hpp"
#include "Inst.hpp"
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <queue>

namespace f1 {

class ClusterModel {
public:
    ClusterModel(const F1TargetMachine& target_machine, int cluster_id);

    // Simulates the instructions cycle-by-cycle and returns a time series map of issued instructions
    TemporalSchedulerResult schedule(const std::vector<Inst>& instructions, F1ModelContext& context, const std::map<std::string, MemoryLocation>& mem_locations, std::string working_dir = "", bool enable_logging = false);

private:
    const F1TargetMachine& _target_machine;
    int _cluster_id;

    // 2. Pipeline State Tracking: 
    // These vectors track the availability (free cycle) of each hardware unit instance
    std::vector<uint64_t> _ntt_units;
    std::vector<uint64_t> _mm_units;
    std::vector<uint64_t> _ma_units;
    
    // Tracks completion times of in-flight loads to model MSHRs (Miss Status Holding Registers)
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> _inflight_loads;
    uint64_t _load_port_free_cycle; // Tracks when the single load issue port is available

    // Helper function to find the first available functional unit for the current cycle
    int getAvailableUnit(const std::vector<uint64_t>& units, uint64_t current_cycle) const;
};

} // namespace f1