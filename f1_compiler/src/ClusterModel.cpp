#include "ClusterModel.hpp"
#include "RegisterNode.hpp"
#include <algorithm>
#include <iostream>

namespace f1 {

ClusterModel::ClusterModel(const F1TargetMachine& target_machine, int cluster_id)
    : _target_machine(target_machine), _cluster_id(cluster_id) {
    
    auto& cluster_cfg = _target_machine.micro_arch_cfg.cluster_config;
    
    _ntt_units.resize(cluster_cfg.num_NTT, 0);
    _mm_units.resize(cluster_cfg.num_MM, 0);
    _ma_units.resize(cluster_cfg.num_MA, 0);
    
    // Assume at least 1 load port for the cluster if not explicitly modeled in config yet
    _load_units.resize(1, 0); 
}

int ClusterModel::getAvailableUnit(const std::vector<uint64_t>& units, uint64_t current_cycle) const {
    for (int i = 0; i < units.size(); ++i) {
        if (units[i] <= current_cycle) {
            return i;
        }
    }
    return -1;
}

std::map<uint64_t, std::vector<Inst>> ClusterModel::schedule(const std::vector<Inst>& instructions, F1ModelContext& context) {
    // 5. Time Series Instruction Map
    std::map<uint64_t, std::vector<Inst>> time_series_inst_map;
    
    // 1. Dependency Tracking
    std::map<std::string, uint64_t> data_ready_cycle;
    
    auto cluster_cfg = _target_machine.micro_arch_cfg.cluster_config;
    uint64_t reg_size = _target_machine.micro_arch_cfg.vector_register_size;
    
    std::vector<Inst> pending_insts = instructions;
    uint64_t current_cycle = 0;

    while (!pending_insts.empty()) {
        std::vector<int> issued_indices;
        
        // 3. Issue Logic
        for (int i = 0; i < pending_insts.size(); ++i) {
            auto& inst = pending_insts[i];
            bool ready = true;

            // Check if all source operands are ready (Local dependencies resolved)
            for (const auto& [node_id, identity] : inst.getSrcLabels()) {
                if (data_ready_cycle.count(identity) && data_ready_cycle[identity] > current_cycle) {
                    ready = false;
                    break;
                }
            }

            if (!ready) continue;

            /*
             * ==============================================================================
             * 4. Priority Heuristics (Local Critical Path)
             * ==============================================================================
             * To minimize pipeline conflicts and overall latency, a Ready-List Scheduler 
             * should prioritize instructions based on their criticality instead of sequence ID.
             * 
             * Implementation Strategy:
             * 1. Build a local dependency graph mapping outputs to dependents before the loop.
             * 2. Recursively calculate the "Longest Path to Exit" (critical path weight) 
             *    for each instruction using simulated latencies.
             * 3. Instead of iterating `pending_insts` linearly, sort the "Ready List" by:
             *    a. Maximum Critical Path Weight (Primary priority).
             *    b. Maximum Registers Freed (Secondary priority to reduce register pressure).
             * 4. Issue the highest priority instruction whose required ALU is free.
             * ==============================================================================
             */

            InstId id = inst.getId();
            
            // Retrieve the dynamic register span (N) directly from the RegisterNode to model ALU occupancy
            uint64_t register_span = 1;
            auto dst_node = context.dag->getNode(inst.getDstLabel().first);
            if (auto reg_node = std::dynamic_pointer_cast<RegisterNode>(dst_node)) {
                auto span = reg_node->getRegSpan();
                register_span = span.second - span.first + 1;
            } else {
                uint64_t data_size = dst_node->dataSize();
                register_span = (data_size + reg_size - 1) / reg_size;
                if (register_span == 0) register_span = 1;
            }

            int unit_idx = -1;
            uint32_t latency = 1; // Default latency safeguard
            bool fully_pipeline = true;
            uint64_t occupancy = register_span;

            // Request the appropriate hardware pipeline based on instruction type
            if (id == InstId::NTT || id == InstId::INTT) {
                unit_idx = getAvailableUnit(_ntt_units, current_cycle);
                latency = cluster_cfg.ntt_config.latency;
                fully_pipeline = cluster_cfg.ntt_config.fully_pipeline;
                if (unit_idx != -1) _ntt_units[unit_idx] = current_cycle + (fully_pipeline ? 1 : latency);
                occupancy = fully_pipeline ? register_span : (register_span * latency);
                if (unit_idx != -1) _ntt_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::ModMul) {
                unit_idx = getAvailableUnit(_mm_units, current_cycle);
                latency = cluster_cfg.mm_config.latency;
                fully_pipeline = cluster_cfg.mm_config.fully_pipeline;
                if (unit_idx != -1) _mm_units[unit_idx] = current_cycle + (fully_pipeline ? 1 : latency);
                occupancy = fully_pipeline ? register_span : (register_span * latency);
                if (unit_idx != -1) _mm_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::ModAdd) {
                unit_idx = getAvailableUnit(_ma_units, current_cycle);
                latency = cluster_cfg.ma_config.latency;
                fully_pipeline = cluster_cfg.ma_config.fully_pipeline;
                if (unit_idx != -1) _ma_units[unit_idx] = current_cycle + (fully_pipeline ? 1 : latency);
                occupancy = fully_pipeline ? register_span : (register_span * latency);
                if (unit_idx != -1) _ma_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::Load) {
                unit_idx = getAvailableUnit(_load_units, current_cycle);
                latency = 1; // Assuming 1 cycle load from scratchpad to register
                if (unit_idx != -1) _load_units[unit_idx] = current_cycle + 1;
                latency = 1; 
                occupancy = register_span; // Assuming 1 cycle load per vector register from scratchpad
                if (unit_idx != -1) _load_units[unit_idx] = current_cycle + occupancy;
            } else {
                // Miscellaneous instructions (assuming 1 cycle, always available)
                unit_idx = 0; 
                latency = 1;
                occupancy = 1;
            }

            // If an ALU was successfully allocated, commit the issue
            if (unit_idx != -1) {
                time_series_inst_map[current_cycle].push_back(inst);
                
                auto [dst_node_id, dst_identity] = inst.getDstLabel();
                data_ready_cycle[dst_identity] = current_cycle + latency;
                uint64_t ready_time = current_cycle + occupancy + (fully_pipeline ? (latency > 0 ? latency - 1 : 0) : 0);
                data_ready_cycle[dst_identity] = ready_time;
                
                issued_indices.push_back(i);
            }
        }

        // Remove issued instructions from pending queue (reverse order to avoid shifting issues)
        for (int i = issued_indices.size() - 1; i >= 0; --i) {
            pending_insts.erase(pending_insts.begin() + issued_indices[i]);
        }

        // Increment simulator cycle
        current_cycle++;
    }

    return time_series_inst_map;
}

} // namespace f1