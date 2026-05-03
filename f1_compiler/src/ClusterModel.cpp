#include "ClusterModel.hpp"
#include "Inst.hpp"
#include "RegisterNode.hpp"
#include "format.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <functional>

namespace f1 {

ClusterModel::ClusterModel(const F1TargetMachine& target_machine, int cluster_id)
    : _target_machine(target_machine), _cluster_id(cluster_id), _load_port_free_cycle(0) {
    
    auto& cluster_cfg = _target_machine.micro_arch_cfg.cluster_config;
    
    _ntt_units.resize(cluster_cfg.num_NTT, 0);
    _mm_units.resize(cluster_cfg.num_MM, 0);
    _ma_units.resize(cluster_cfg.num_MA, 0);
    
    // _inflight_loads is initialized empty by default.
}

int ClusterModel::getAvailableUnit(const std::vector<uint64_t>& units, uint64_t current_cycle) const {
    for (int i = 0; i < units.size(); ++i) {
        if (units[i] <= current_cycle) {
            return i;
        }
    }
    return -1;
}

TemporalSchedulerResult ClusterModel::schedule(const std::vector<Inst>& instructions, F1ModelContext& context, const std::map<std::string, MemoryLocation>& mem_locations, std::string working_dir, bool enable_logging) {
    if (instructions.empty()) {
        return {{}, 0};
    }

    const std::vector<Inst>& transformed_instructions = instructions;


    //* reassign the seq id
    // for (size_t i = 0; i < instructions.size(); ++i) {
    //     transformed_instructions[i].setInstSeqId(i);
    // }

    if (!working_dir.empty() && working_dir.back() != '/') {
        working_dir += '/';
    }

    std::ofstream log_file;
    if (enable_logging) {
        std::string log_filename = working_dir + "cluster_" + std::to_string(_cluster_id) + "_schedule.log";
        log_file.open(log_filename);
        log_file << "=== Cluster " << _cluster_id << " Schedule Log ===\n";
    }

    // --- 1. Build Dependency Graph ---
    // This graph tracks dependencies between instructions *within this cluster*.
    // An instruction is a "root" if all its sources come from outside this cluster (e.g., via Load).
    std::map<uint64_t, size_t> dst_node_id_to_inst_idx;
    std::vector<int> in_degree(transformed_instructions.size());
    std::vector<std::vector<size_t>> successors(transformed_instructions.size());

    for (size_t i = 0; i < transformed_instructions.size(); ++i) {
        dst_node_id_to_inst_idx[transformed_instructions[i].getDstLabel().first] = i;
    }

    for (size_t i = 0; i < transformed_instructions.size(); ++i) {
        int num_deps = 0;
        for (const auto& src : transformed_instructions[i].getSrcLabels()) {
            auto it = dst_node_id_to_inst_idx.find(src.first);
            if (it != dst_node_id_to_inst_idx.end()) {
                size_t producer_idx = it->second;
                successors[producer_idx].push_back(i);
                num_deps++;
            }
        }
        in_degree[i] = num_deps;
    }


    // generate a print instruction lambda function like the function inside report
    auto printInst = [&context](const Inst& inst, uint32_t allocated_cluster_id) {
        std::stringstream ss;
        auto dag = context.dag;
        ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
        ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << identity;
                if (src_id != src_labels.size() - 1) {
                    ss << ", ";
                }
                src_id++;
            }
        }
        ss.str().length() < 100 ? ss << std::string(100 - ss.str().length(), ' ') : ss;
        ss << "  # ";
        ss << std::format("{:<7}", ToInstStr(inst.getId()));
        ss << dag->getNode(inst.getDstLabel().first)->getSymbolicName() << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << dag->getNode(node_id)->getSymbolicName();
                if (src_id != src_labels.size() - 1) {
                    ss << ", ";
                }
                src_id++;
            }
        }
        return ss.str();
    };


    // --- 2. Initialize Scheduler State ---
    std::map<uint64_t, std::vector<Inst>> time_series_inst_map;
    auto cluster_cfg = _target_machine.micro_arch_cfg.cluster_config;
    uint64_t reg_size = _target_machine.micro_arch_cfg.vector_register_size;

    // Min-heap for ready instructions: {data_ready_cycle, instruction_index}
    using ReadyEntry = std::pair<uint64_t, size_t>;
    std::priority_queue<ReadyEntry, std::vector<ReadyEntry>, std::greater<ReadyEntry>> ready_queue;
    std::vector<uint64_t> bank_free_cycle(_target_machine.micro_arch_cfg.scratchpad_bank, 0);

    // Find initial ready instructions (in-degree == 0)
    for (size_t i = 0; i < transformed_instructions.size(); ++i) {
        if (in_degree[i] == 0) {
            if (enable_logging) log_file << "Initial ready inst: " << printInst(transformed_instructions[i], _cluster_id) << "\n";
            ready_queue.push({0, i});
        }
    }

    // --- 3. Hazard and Dependency Tracking ---
    struct HazardInfo {
        uint64_t seq_id;
        uint64_t arrival_cycle;
        uint64_t ready_cycle;
        uint64_t issue_cycle;
        uint64_t bubble;
        uint64_t backpressure;
    };
    std::vector<HazardInfo> hazard_reports;
    std::map<uint64_t, uint64_t> result_issue_cycles; // node_id -> issue_cycle
    std::map<uint64_t, uint64_t> result_ready_cycles; // node_id -> ready_cycle

    // traverse all instructions for literal node
    for (const auto& inst : transformed_instructions) {

        for (auto [node_id, identity] : inst.getSrcLabels()) {
            if (context.dag->getNode(node_id)->isLiteral()) {
                result_issue_cycles[node_id] = 0;
                result_ready_cycles[node_id] = 0;
            }
        }
    }


    // --- 4. Main Scheduling Loop (List Scheduling Algorithm) ---
    uint64_t current_cycle = 0;
    size_t num_scheduled = 0;
    std::vector<size_t> pending_issue_indices;
    uint64_t last_time_issued_cycles = 0;

    // --- Schedule Logging Initialization ---
    while (num_scheduled < transformed_instructions.size()) {
        // Retire completed in-flight loads
        while (!_inflight_loads.empty() && _inflight_loads.top() <= current_cycle) {
            _inflight_loads.pop();
        }

        // Add newly data-ready instructions to the pending list
        while (!ready_queue.empty() && ready_queue.top().first <= current_cycle) {
            pending_issue_indices.push_back(ready_queue.top().second);
            ready_queue.pop();
        }

        // Prioritize instructions in the pending list (e.g., by original sequence ID as a tie-breaker)
        std::sort(pending_issue_indices.begin(), pending_issue_indices.end(),
                    [&](auto a, auto b){
                        return transformed_instructions[a].getInstSeqId() < transformed_instructions[b].getInstSeqId();
                    });

        // Log current cycle and pending instructions
        // log_file << "Cycle " << current_cycle << " | Pending Insts: ";
        // if (pending_issue_indices.empty()) {
        //     // log_file << "None";
        // } else {
        //     // log_file << "Pending issue insts: " << pending_issue_indices.size();
        //     // for (size_t i = 0; i < pending_issue_indices.size(); ++i) {
        //     //     const auto& inst = instructions[pending_issue_indices[i]];
        //     //     log_file << inst.getInstSeqId() << "(" << ToInstStr(inst.getId()) << ")";
        //     //     if (i != pending_issue_indices.size() - 1) log_file << ", ";
        //     // }
        // }
        // log_file << "\n";

        std::vector<int> issued_pending_indices; // Stores indices *into* pending_issue_indices

        for (int i = 0; i < pending_issue_indices.size(); ++i) {
            size_t inst_idx = pending_issue_indices[i];
            const auto& inst = transformed_instructions[inst_idx];

            // Retrieve the dynamic register span (N) to model ALU occupancy
            uint64_t register_span = 1;
            auto dst_node = context.dag->getNode(inst.getDstLabel().first);
            if (auto reg_node = std::dynamic_pointer_cast<RegisterNode>(dst_node)) {
                auto span = reg_node->getRegSpan();
                register_span = span.second - span.first + 1;
            } else {
                register_span = (dst_node->dataSize() + reg_size - 1) / reg_size;
                if (register_span == 0) register_span = 1;
            }

            // log_file << "inst id: " << ToInstStr(inst.getId());
            // log_file << "register span: " << register_span << "\n";

            int unit_idx = -1;
            uint32_t latency = 1;
            bool fully_pipeline = true;
            uint64_t occupancy = register_span;
            uint64_t dst_ready_time;

            // Request the appropriate hardware pipeline based on instruction type
            InstId id = inst.getId();
            if (id == InstId::NTT || id == InstId::INTT) {
                unit_idx = getAvailableUnit(_ntt_units, current_cycle);
                latency = cluster_cfg.ntt_config.latency;
                fully_pipeline = cluster_cfg.ntt_config.fully_pipeline;
                occupancy = fully_pipeline ? register_span + latency : (register_span * latency);
                if (unit_idx != -1) dst_ready_time = _ntt_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::ModMul) {
                unit_idx = getAvailableUnit(_mm_units, current_cycle);
                latency = cluster_cfg.mm_config.latency;
                fully_pipeline = cluster_cfg.mm_config.fully_pipeline;
                occupancy = fully_pipeline ? register_span + latency : (register_span * latency);
                if (unit_idx != -1) dst_ready_time = _mm_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::ModAdd) {
                unit_idx = getAvailableUnit(_ma_units, current_cycle);
                latency = cluster_cfg.ma_config.latency;
                fully_pipeline = cluster_cfg.ma_config.fully_pipeline;
                occupancy = fully_pipeline ? register_span + latency : (register_span * latency);
                if (unit_idx != -1) dst_ready_time = _ma_units[unit_idx] = current_cycle + occupancy;
            } else if (id == InstId::Load) {
                uint32_t bank_id = 0; // Default if not found
                if (!inst.getSrcLabels().empty()) {
                    auto it = mem_locations.find(inst.getSrcLabels()[0].second);
                    if (it != mem_locations.end()) {
                        bank_id = it->second.bank_id;
                    }
                }

                // Model MSHRs: check if we can accept a new outstanding load request
                // Also model a single-issue load port and bank conflicts.
                if (_inflight_loads.size() < cluster_cfg.num_mshr && current_cycle >= _load_port_free_cycle && current_cycle >= bank_free_cycle[bank_id]) {
                    unit_idx = 1; // A dummy value to indicate success
                } else {
                    unit_idx = -1; // MSHRs full, port is busy, or bank conflict, cannot issue
                }

                latency = 1; // Assume 1 cycle to issue
                occupancy = register_span;
                if (unit_idx != -1) {
                    dst_ready_time = current_cycle + occupancy; // Data is ready after transfer
                    _inflight_loads.push(dst_ready_time);
                    _load_port_free_cycle = current_cycle + 1; // Port is busy for this cycle
                    bank_free_cycle[bank_id] = current_cycle + occupancy; // Bank is busy transferring data
                }
            } else {
                throw std::runtime_error("Not supported instruction type: " + ToInstStr(id));
            }

            // If an ALU was successfully allocated, commit the issue
            if (unit_idx != -1) {
                if (enable_logging) log_file << "Issued: " << inst.getInstSeqId() << "(" << ToInstStr(inst.getId()) << ")" << " complete time: " << dst_ready_time << "\n";

                time_series_inst_map[current_cycle].push_back(inst);
                issued_pending_indices.push_back(i);
                num_scheduled++;

                // Calculate result ready time and update dependencies
                auto [dst_node_id, dst_identity] = inst.getDstLabel();
                // uint64_t dst_ready_time = current_cycle + occupancy + (fully_pipeline ? (latency > 0 ? latency - 1 : 0) : 0);
                result_ready_cycles[dst_node_id] = dst_ready_time;
                result_issue_cycles[dst_node_id] = current_cycle;
                last_time_issued_cycles = current_cycle;

                // Track Hazard Metrics
                HazardInfo info;
                info.seq_id = inst.getInstSeqId();
                info.issue_cycle = current_cycle;
                uint64_t max_src_ready = 0;
                uint64_t max_src_issue = 0;
                for (const auto& src : inst.getSrcLabels()) {
                    if (result_ready_cycles.count(src.first)) {
                        max_src_ready = std::max(max_src_ready, result_ready_cycles.at(src.first));
                        max_src_issue = std::max(max_src_issue, result_issue_cycles.at(src.first));
                    }
                }
                info.arrival_cycle = max_src_issue;
                info.ready_cycle = max_src_ready;
                info.bubble = (info.ready_cycle > info.arrival_cycle) ? (info.ready_cycle - info.arrival_cycle) : 0;
                info.backpressure = (info.issue_cycle > info.ready_cycle) ? (info.issue_cycle - info.ready_cycle) : 0;
                hazard_reports.push_back(info);

                // Update successors, adding them to the ready queue if all their dependencies are met

                for (auto suc_it = successors[inst_idx].begin(); suc_it != successors[inst_idx].end(); ++suc_it) {
                    auto successor_idx = *suc_it;
                    in_degree[successor_idx]--;
                    if (in_degree[successor_idx] == 0) {
                        uint64_t max_src_ready_time = 0;
                        const auto& successor_inst = transformed_instructions[successor_idx];
                        for (const auto& src : successor_inst.getSrcLabels()) {
                            if (result_ready_cycles.count(src.first)) {
                                max_src_ready_time = std::max(max_src_ready_time, result_ready_cycles.at(src.first));
                            } else {
                                throw std::logic_error("Found successor degree 0, but without ready time");
                            }
                        }
                        ready_queue.push({max_src_ready_time, successor_idx});
                    }
                }
                for (size_t successor_idx : successors[inst_idx]) {
                    in_degree[successor_idx]--;
                    if (in_degree[successor_idx] == 0) {
                        uint64_t max_src_ready_time = 0;
                        const auto& successor_inst = transformed_instructions[successor_idx];
                        for (const auto& src : successor_inst.getSrcLabels()) {
                            if (result_ready_cycles.count(src.first)) {
                                max_src_ready_time = std::max(max_src_ready_time, result_ready_cycles.at(src.first));
                            }
                        }
                        ready_queue.push({max_src_ready_time, successor_idx});
                    }
                }
            } else {
                // log_file << "Failed to issue: " << inst.getInstSeqId() << "(" << ToInstStr(inst.getId()) << ")" << " "; 
            }
        }


        // Remove issued instructions from the pending list (in reverse to not invalidate indices)
        for (int i = issued_pending_indices.size() - 1; i >= 0; --i) {
            pending_issue_indices.erase(pending_issue_indices.begin() + issued_pending_indices[i]);
        }

        // log_file << "Cycle " << current_cycle << " ended" << "\n";
        current_cycle++;
        if (current_cycle - last_time_issued_cycles > 1000) {
            if (enable_logging) {
                log_file << "The instruction hasn't issued for " + std::to_string(last_time_issued_cycles) << "remained instructions number: " << num_scheduled << "\n";
                // print pending issue instructions
                for (int i = 0; i < pending_issue_indices.size(); ++i) {
                    const auto& inst = transformed_instructions[pending_issue_indices[i]];
                    log_file << "still have inst " << printInst(inst, _cluster_id) << "\n";
                }
    
                for (auto suc_vec : successors) {
                    for (auto suc : suc_vec) {
                        log_file << "succ indegree: " << in_degree[suc] << ", " << printInst(transformed_instructions[suc], _cluster_id) << "\n";
                    }
                }
            }

            break;
        }
        // log_file << "\n";
    }

    // --- 5. Output Hazard Report ---
    if (enable_logging) {
        std::string filename = working_dir + "cluster_" + std::to_string(_cluster_id) + "_hazards.csv";
        std::ofstream ofs(filename);
        ofs << "SeqID,ArrivalCycle,ReadyCycle,IssueCycle,Bubble(DataWait),Backpressure(ALUWait)\n";
        for (const auto& info : hazard_reports) {
            ofs << info.seq_id << ","
                << info.arrival_cycle << ","
                << info.ready_cycle << ","
                << info.issue_cycle << ","
                << info.bubble << ","
                << info.backpressure << "\n";
        }
        ofs.close();
        std::cout << "[Cluster " << _cluster_id << "] Hazard metrics written to " << filename << std::endl;
    
        if (log_file.is_open()) {
            log_file.close();
        }
    }

    uint64_t total_cycles = 0;
    for (const auto& [node_id, ready_cycle] : result_ready_cycles) {
        total_cycles = std::max(total_cycles, ready_cycle);
    }


    return {time_series_inst_map, total_cycles};
}

} // namespace f1