#pragma once

#include "F1Model.hpp"
#include "Inst.hpp"
#include <vector>
#include <map>
#include <string>
#include <cstdint>

namespace f1 {

class ClusterModel {
public:
    ClusterModel(const F1TargetMachine& target_machine, int cluster_id);

    // Simulates the instructions cycle-by-cycle and returns a time series map of issued instructions
    std::map<uint64_t, std::vector<Inst>> schedule(const std::vector<Inst>& instructions, F1ModelContext& context);

private:
    const F1TargetMachine& _target_machine;
    int _cluster_id;

    // 2. Pipeline State Tracking: 
    // These vectors track the availability (free cycle) of each hardware unit instance
    std::vector<uint64_t> _ntt_units;
    std::vector<uint64_t> _mm_units;
    std::vector<uint64_t> _ma_units;
    
    // Basic load unit tracker to restrict memory/scratchpad bandwidth per cycle
    std::vector<uint64_t> _load_units; 

    // Helper function to find the first available functional unit for the current cycle
    int getAvailableUnit(const std::vector<uint64_t>& units, uint64_t current_cycle) const;
};

} // namespace f1