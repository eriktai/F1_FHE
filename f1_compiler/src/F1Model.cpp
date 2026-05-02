#include "F1Model.hpp"
#include "DAG.hpp"
#include "MemoryAllocator.hpp"
#include "RegisterNode.hpp"
#include "ClusterModel.hpp"
#include <iostream>
#include <stdexcept>


namespace f1 {

F1Model::F1Model(F1TargetMachine target_machine)
    : _target_machine(std::move(target_machine))
    {} 

std::vector<uint32_t> SpatialScheduler::evaluateDataTemporalLocality(const std::map<uint32_t, uint32_t>& candidate_cluster_counts, int num_clusters) {
    std::vector<uint32_t> candidates;
    if (candidate_cluster_counts.empty()) {
        for (int i = 0; i < num_clusters; ++i) {
            candidates.push_back(i);
        }
    } else {
        uint32_t max_count = 0;
        for (auto const& [cid, count] : candidate_cluster_counts) {
            if (count > max_count) {
                max_count = count;
            }
        }
        for (auto const& [cid, count] : candidate_cluster_counts) {
            if (count == max_count) {
                candidates.push_back(cid);
            }
        }
    }
    return candidates;
}

std::vector<uint32_t> SpatialScheduler::evaluatePipelineLoad(const Inst& inst, const std::vector<uint32_t>& candidates, const std::map<uint32_t, std::vector<Inst>>& cluster_instructions_map) {
    auto getPipelineType = [](InstId id) {
        if (id == InstId::NTT || id == InstId::INTT) return 1;
        if (id == InstId::ModMul) return 2;
        if (id == InstId::ModAdd) return 3;
        return 0; // Miscellaneous (e.g., Load)
    };
    
    int target_pipeline = getPipelineType(inst.getId());
    std::vector<uint32_t> refined_candidates;
    uint32_t min_load = UINT32_MAX;

    for (uint32_t cid : candidates) {
        uint32_t current_pipeline_load = 0;
        
        auto it = cluster_instructions_map.find(cid);
        if (it != cluster_instructions_map.end()) {
            for (const auto& scheduled_inst : it->second) {
                if (getPipelineType(scheduled_inst.getId()) == target_pipeline) {
                    current_pipeline_load++;
                }
            }
        }

        if (current_pipeline_load < min_load) {
            min_load = current_pipeline_load;
            refined_candidates.clear();
            refined_candidates.push_back(cid);
        } else if (current_pipeline_load == min_load) {
            refined_candidates.push_back(cid);
        }
    }
    return refined_candidates;
}

uint32_t SpatialScheduler::clusterAllocation(const Inst& inst, const std::map<uint32_t, uint32_t>& candidate_cluster_counts, const std::map<uint32_t, std::vector<Inst>>& cluster_instructions_map, int num_clusters) {
    
    // Priority 1: Data Temporal Locality
    std::vector<uint32_t> primary_candidates = evaluateDataTemporalLocality(candidate_cluster_counts, num_clusters);

    if (primary_candidates.size() == 1) {
        return primary_candidates[0];
    }

    // Priority 2: Pipeline Load Balancing (Evaluate tied candidates)
    std::vector<uint32_t> secondary_candidates = evaluatePipelineLoad(inst, primary_candidates, cluster_instructions_map);

    if (secondary_candidates.size() == 1) {
        return secondary_candidates[0];
    }

    // Tie-breaker: Overall cluster load
    uint32_t best_cluster = secondary_candidates[0];
    uint32_t min_total_load = UINT32_MAX;

    for (uint32_t cid : secondary_candidates) {
        uint32_t total_load = 0;
        auto it = cluster_instructions_map.find(cid);
        if (it != cluster_instructions_map.end()) {
            total_load = it->second.size();
        }

        if (total_load < min_total_load) {
            min_total_load = total_load;
            best_cluster = cid;
        }
    }

    return best_cluster;
}

SpatialMappingResult SpatialScheduler::schedule(const std::vector<Inst>& instructions, F1ModelContext& context) {
    //* start the spatial mapping
    int num_clusters = context.target_machine->micro_arch_cfg.num_clusters;
    SpatialMappingResult result;
    auto& cluster_instructions_map = result.cluster_instructions_map;
    auto& dst_data_cluster_map = result.data_cluster_map;

    for (const auto& inst : instructions) {

        auto& src_labels = inst.getSrcLabels();

        // ------------------------------------------------------------------
        // Phase 1: Dependency Tracking
        // Identify which clusters hold the data required by this instruction.
        // ------------------------------------------------------------------
        std::map<uint32_t, uint32_t> candidate_cluster_counts;
        for (auto& [node_id, identity] : src_labels) {
            auto src_node = context.dag->getNode(node_id);
            if (src_node->isLiteral()) continue;
            
            if (dst_data_cluster_map.count(identity)) {
                candidate_cluster_counts[dst_data_cluster_map[identity]]++;
            }
        }

        // ------------------------------------------------------------------
        // Phase 2: Cluster Allocation
        // Assign the instruction to a cluster to minimize data movement.
        // ------------------------------------------------------------------
        uint32_t allocated_cluster_id;
        auto dest_node = context.dag->getNode(inst.getDstLabel().first);
        if (dest_node->hasClusterHint()) {
            allocated_cluster_id = dest_node->getClusterHint() % num_clusters;
        } else {
            allocated_cluster_id = clusterAllocation(inst, candidate_cluster_counts, cluster_instructions_map, num_clusters);
        }

        // ------------------------------------------------------------------
        // Phase 3: Instruction Routing and Data Loading
        // Generate necessary Load instructions to bring remote data into
        // the allocated cluster before pushing the main instruction.
        // ------------------------------------------------------------------
        auto genLoadInstFor = [&allocated_cluster_id](const Inst& inst, const NodeDescriptor& src_nd) {
            auto& [node_id, identity] = src_nd;
            std::string dst_identity = std::format("C{}::reg[NULL]", allocated_cluster_id);
            Inst ld_inst = Inst(ToInstId("Load"), {node_id, dst_identity}, {{node_id, identity}});
            ld_inst.setInstSeqId(inst.getInstSeqId());
            return ld_inst;
        };

        for (auto& [src_node_id, src_identity] : src_labels) {
            auto src_node = context.dag->getNode(src_node_id);
            if (src_node->isLiteral()) continue;

            auto it = dst_data_cluster_map.find(src_identity);
            if (it == dst_data_cluster_map.end()) {
                // Data is not in any cluster (e.g., in memory/scratchpad). Load it.
                cluster_instructions_map[allocated_cluster_id].push_back(genLoadInstFor(inst, {src_node_id, src_identity}));
                dst_data_cluster_map[src_identity] = allocated_cluster_id;
            } else if (it->second != allocated_cluster_id) {
                // Data exists, but in a different cluster. Load it over the network.
                cluster_instructions_map[allocated_cluster_id].push_back(genLoadInstFor(inst, {src_node_id, src_identity}));
            }
        }

        cluster_instructions_map[allocated_cluster_id].push_back(inst);
        dst_data_cluster_map[inst.getDstLabel().second] = allocated_cluster_id;
    }

    return result;
}

void F1Model::report(const std::vector<std::vector<Inst>>& cluster_instructions_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, const std::vector<RegisterFile>& reg_files, std::string outpath) {

    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
    const auto& dst_data_cluster_map = data_cluster_map;
    //* generate instruction sequence
    std::map<uint64_t, uint64_t> data_mv_count_between_clusters_map;
    std::map<uint64_t, std::vector<std::string>> cluster_data_mv_map;
    std::vector<std::vector<uint64_t>> data_mv_count_map(num_clusters, std::vector<uint64_t>(num_clusters, 0)); //* data_mv_count_map[src_cluster][dst_cluster]
    std::vector<uint64_t> data_mv_from_scratchpad_count(num_clusters, 0);

    auto printInst = [&context](const Inst& inst, uint32_t allocated_cluster_id, const std::map<std::string, uint32_t>& dst_data_cluster_map) {
        std::stringstream ss;
        auto dag = context.dag;
        ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
        ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << identity;
                if (dst_data_cluster_map.count(identity)) {
                    ss << "(C" << dst_data_cluster_map.at(identity) << ")";
                } else {
                    ss << "(NULL)";
                }
                if (src_id != src_labels.size() - 1) {
                    ss << ", ";
                }
                src_id++;
            }

        }
        ss.str().length() < 100 ? ss << std::string(100 - ss.str().length(), ' ') : ss;
        //* print annotate
        ss << "  # ";
        ss << std::format("{:<7}", ToInstStr(inst.getId()));
        auto dst_node = dag->getNode(inst.getDstLabel().first);
        ss << dst_node->getSymbolicName() << ", ";
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
        if (dst_node->hasClusterHint()) {
            ss << ", Cluster Hint: " << dst_node->getClusterHint();
        } else {
            ss << ", No Cluster Hint:";
        }
        return ss.str();
    };


    std::ofstream fs(outpath);
    assert(num_clusters == cluster_instructions_map.size());
    for (int i = 0 ; i < num_clusters; i++) {
        fs << "Cluster " << i << ":\n";
        uint64_t num_data_movement_between_clusters = 0;
        for (auto& inst : cluster_instructions_map[i]) {
            fs << printInst(inst, i, dst_data_cluster_map) << std::endl;
            
            if (inst.getId() == InstId::Load) {
                data_mv_from_scratchpad_count[i]++;
            }
            // else {
            for (auto& [node_id, identity] : inst.getSrcLabels()) {
                if (context.dag->getNode(node_id)->isLiteral()) continue;
                if (dst_data_cluster_map.count(identity) ) {
                    if (dst_data_cluster_map.at(identity) != i) {
                        num_data_movement_between_clusters++;
                    }
                    data_mv_count_map[dst_data_cluster_map.at(identity)][i]++;
                    // std::string mv_key = std::format("C{}->C{}", dst_data_cluster_map[identity], i);
                }
            } 
            // }
        }
        data_mv_count_between_clusters_map[i] = num_data_movement_between_clusters;
        //* report 
        //* data movement between clusters
        fs << "\n\n";
    }
    
    fs << "Data movement between clusters:\n";
    fs << "From\\To, ";
    for (int i = 0; i < num_clusters; i++) {
        fs << std::format("{:<3} ", i);
    }
    fs << "\n";
    for (int i = 0; i < num_clusters; i++) {
        fs << std::format("{:<7} |", i);
        for (int j = 0; j < num_clusters; j++) {
            fs << std::format("{:<3} ", data_mv_count_map[i][j]);
        }
        fs << "\n";
    }

    uint64_t coeff_vec_size = _target_machine.software_cfg.degree * 4;

    fs << "\n\nData movement count between clusters:\n";
    for (auto& [cluster_id, mv_count] : data_mv_count_between_clusters_map) {
        fs << std::format("Cluster {}: {} data movements, total size: {} KB\n", cluster_id, mv_count, mv_count * coeff_vec_size / 1024);
    }
    fs << "\n";

    fs << "\n\nData movement count from scratchpad:\n";
    for (int i = 0; i < num_clusters; i++) {
        fs << std::format("Cluster {}: {} data movements from scratchpad, total size: {} MB\n", i, data_mv_from_scratchpad_count[i], data_mv_from_scratchpad_count[i] * coeff_vec_size / 1024 / 1024);
    };
    fs << "\n";

    fs << "\n\nInstruction numbers in each cluster:\n";
    for (int i = 0; i < num_clusters; i++) {
        fs << std::format("Cluster {}: {} instructions\n", i, cluster_instructions_map[i].size());
    }
    fs << "\n";
    fs << "\n\nRegister utilization in each cluster:\n";
    fs << analyzeRegisterUtilization(reg_files, 4);
    fs << "\n";
}

std::vector<Inst> RegisterAllocator::allocate(int cluster_id, const std::vector<Inst>& instructions, const DataLivenessMap& liveness_map, std::map<std::string, uint32_t>& data_cluster_map, RegisterFile& reg_file, F1ModelContext& context) {

    //* Data Node Id to Register Node Ptr
    std::map<uint64_t, RegisterNodePtr> register_nodes_map;

    std::vector<Inst> transformed_instructions;

    for (uint64_t inst_i = 0; inst_i < instructions.size(); inst_i++) {
    // for (auto inst : instructions) {
        auto inst = instructions[inst_i];
        // auto cur_seq_id = inst.getInstSeqId();

        // ------------------------------------------------------------------
        // 1. Liveness Expiration
        // Scan all active registers and release those whose lifespan has ended.
        // ------------------------------------------------------------------
        for (auto it = register_nodes_map.begin(); it != register_nodes_map.end(); ) {
            auto& [effective_id, reg_node] = *it;

            auto liveness = liveness_map.at(effective_id);
            if (inst_i < liveness.first) {
                throw std::runtime_error("Encountered a register allocated before its liveness");
            }
            if (inst_i > liveness.second) {
                reg_file.releaseAllocated(reg_node->getSymbolicId());
                it = register_nodes_map.erase(it);
            } else {
                it++;
            }
        }

        // ------------------------------------------------------------------
        // 2. Load Instructions
        // Allocate space for incoming data loaded from memory/other clusters.
        // ------------------------------------------------------------------
        if (inst.getId() == InstId::Load) {
            auto& src_labels = inst.getSrcLabels();
            if (src_labels.size() > 1) {
                throw std::runtime_error("The source of the load instruction should be always 1");
            }

            auto& [src_node_id, src_identity] = src_labels[0];
            auto reg_span_opt = reg_file.getAllocatedRegsOpt(src_node_id);

            if (!reg_span_opt.has_value()) {
                auto req_size = context.dag->getNode(src_node_id)->dataSize(); //* in bytes 
                if (reg_file.canAllocate(req_size)) {
                    auto src_node = context.dag->getNode(src_node_id);
                    auto allocated_reg_span = reg_file.allocate(src_node_id, req_size); //* assume the register size is 32 bytes
                    auto reg_node = context.dag->allocNodeAs<RegisterNode>(cluster_id, allocated_reg_span, reg_file.getRegSize());
                    reg_node->setSymbolicId(src_node_id);
                    reg_node->setReferenceId(context.dag->getNode(src_node_id)->getEffectiveId());
                    reg_node->setSymbolicName(src_node->getSymbolicName());
                    register_nodes_map[reg_node->getEffectiveId()] = reg_node;

                    inst.setDstLabel(NodeDescriptor{reg_node->getId(), reg_node->getIdentity()});
                    data_cluster_map[reg_node->getIdentity()] = cluster_id;
                } else {
                    throw std::runtime_error(std::format("Cluster {} lacks registers: allocated {}, required {} B", 
                                             cluster_id, reg_file.getAllocatedSize(), req_size));
                }
            } 
        } 
        // ------------------------------------------------------------------
        // 3. Standard Compute Instructions
        // Map source operands to existing registers and allocate destination.
        // ------------------------------------------------------------------
        else {
            auto& src_labels = inst.getSrcLabels();
            std::vector<NodeDescriptor> trans_src_labels;
            for (auto [node_id, identity] : src_labels) {
                auto src_node = context.dag->getNode(node_id);
                auto allocated_regs_opt = reg_file.getAllocatedRegsOpt(src_node->getEffectiveId());
                NodeDescriptor new_nd{node_id, identity};
                if (!allocated_regs_opt.has_value()) {
                    if (cluster_id == data_cluster_map[identity] && !src_node->isLiteral()) {
                        throw std::runtime_error(std::format("Source register not allocated for node_id: {}", src_node->getEffectiveId()));
                    }
                } else {
                    auto it = register_nodes_map.find(context.dag->getNode(node_id)->getEffectiveId());
                    if (it == register_nodes_map.end()) {
                        throw std::runtime_error(std::format("Register node not found - Cluster: {}, Node ID: {}, Ref ID: {}", 
                                                 cluster_id, context.dag->getNode(node_id)->getId(), context.dag->getNode(node_id)->getReferenceId()));
                    }
                    auto reg_node = it->second;
                    new_nd = {reg_node->getId(), reg_node->getIdentity()};
                }
                trans_src_labels.push_back(new_nd);
            }
            inst.setSrcLabels(trans_src_labels);

            auto [dst_node_id, dst_identity] = inst.getDstLabel(); 
            auto dst_node = context.dag->getNode(dst_node_id);
            auto req_size = dst_node->dataSize();
            auto reg_span_opt = reg_file.getAllocatedRegsOpt(dst_node->getEffectiveId());

            if (reg_span_opt.has_value()) {
                if (register_nodes_map.count(dst_node->getEffectiveId()) == 0) {
                    throw std::runtime_error("Cannot find allocated nodes from effected id: " + std::to_string(dst_node->getEffectiveId()));
                }
                auto reg_node = register_nodes_map[dst_node->getEffectiveId()];
                inst.setDstLabel(NodeDescriptor{reg_node->getId(), reg_node->getIdentity()});
            } else {
                if (reg_file.canAllocate(req_size)) {
                    auto allocated_reg_span = reg_file.allocate(dst_node_id, req_size); //* assume the register size is 32 bytes
                    auto reg_node = context.dag->allocNodeAs<RegisterNode>(cluster_id, allocated_reg_span, reg_file.getRegSize());
                    auto effective_id = context.dag->getNode(dst_node_id)->getEffectiveId();
                    reg_node->setSymbolicId(dst_node_id);
                    reg_node->setReferenceId(effective_id);
                    reg_node->setSymbolicName(dst_node->getSymbolicName());
                    register_nodes_map[effective_id] = reg_node;
                    inst.setDstLabel(NodeDescriptor{reg_node->getId(), reg_node->getIdentity()});
                    data_cluster_map[reg_node->getIdentity()] = cluster_id;
                } else {
                    throw std::runtime_error(std::format("Cluster {} lacks registers: allocated {}, required {} B", 
                                             cluster_id, reg_file.getAllocatedSize(), req_size));
                }
            }
        }

        transformed_instructions.push_back(inst);
    }

    return transformed_instructions;
}

DataLivenessMap LivenessAnalyzer::compute(const std::vector<Inst>& instructions, F1ModelContext& context) {
    DataLivenessMap liveness_map;

    for (uint64_t i = 0; i < instructions.size(); i++) {
        auto& inst = instructions[i];

        auto& [dst_node_id, dst_identity] = inst.getDstLabel();
        auto effective_id = context.dag->getNode(dst_node_id)->getEffectiveId();
        if (liveness_map.find(effective_id) == liveness_map.end()) {
            liveness_map[effective_id] = {i, i};
        } else {
            liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, i); 
            liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, i);
        }

        for (const auto& [src_node_id, src_identity] : inst.getSrcLabels()) {
            auto effective_id = context.dag->getNode(src_node_id)->getEffectiveId();
            if (liveness_map.find(effective_id) == liveness_map.end()) {
                liveness_map[effective_id] = {i, i};
            } else {
                liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, i); 
                liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, i);
            }
        }        
    }
    // for (auto inst : instructions) {
    //     auto inst_seq_id = inst.getInstSeqId();
    //     //* check dst data liveness
    //     auto& [dst_node_id, dst_identity] = inst.getDstLabel();
    //     auto effective_id = context.dag->getNode(dst_node_id)->getEffectiveId();
    //     if (liveness_map.find(effective_id) == liveness_map.end()) {
    //         liveness_map[effective_id] = {inst_seq_id, inst_seq_id};
    //     } else {
    //         liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, inst_seq_id); 
    //         liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, inst_seq_id);
    //     }

    //     for (const auto& [src_node_id, src_identity] : inst.getSrcLabels()) {
    //         auto effective_id = context.dag->getNode(src_node_id)->getEffectiveId();
    //         if (liveness_map.find(effective_id) == liveness_map.end()) {
    //             liveness_map[effective_id] = {inst_seq_id, inst_seq_id};
    //         } else {
    //             liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, inst_seq_id); 
    //             liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, inst_seq_id);
    //         }
    //     }        
    // }

    return liveness_map;

}

F1ModelResult F1Model::compile(DAG* dag, CipherTextNodePtr root_node, std::string working_dir, bool enable_logging) {
    if (!working_dir.empty() && working_dir.back() != '/') {
        working_dir += '/';
    }

    //* 1. Traverse the DAG to get the instruction sequence
    CodeGenerator code_gen(dag);
    //* travel the DAG and generate instruction sequence
    code_gen.travel(root_node);

    auto& instructions = code_gen.getInstructions();


    F1ModelContext context;
    context.dag = dag;
    context.target_machine = &_target_machine;
    std::vector<RegisterFile> register_files_vec(_target_machine.micro_arch_cfg.num_clusters, 
                                    RegisterFile(_target_machine.micro_arch_cfg.vector_register_num, _target_machine.micro_arch_cfg.vector_register_size));
    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
        
    SpatialScheduler spatial_scheduler(num_clusters);
    auto spatial_result = spatial_scheduler.schedule(instructions, context);

    if (enable_logging) {
        std::vector<std::vector<Inst>> instsList(num_clusters, std::vector<Inst>{});
        for (int i = 0; i < num_clusters; i++) {
            instsList[i] = spatial_result.cluster_instructions_map[i];
        }
        report(instsList, context, spatial_result.data_cluster_map, register_files_vec, working_dir + "schedule_before_reg_alloc.out");
    }

    //* register allocation 
    std::vector<std::vector<Inst>> register_allocated_insts_vec(num_clusters, std::vector<Inst>{});
    LivenessAnalyzer liveness_analyzer;
    RegisterAllocator register_allocator;
    for (int i = 0; i < num_clusters; i++) {
        auto liveness_map = liveness_analyzer.compute(spatial_result.cluster_instructions_map[i], context);
        auto transformed_insts = register_allocator.allocate(i, spatial_result.cluster_instructions_map[i], liveness_map, spatial_result.data_cluster_map, register_files_vec[i], context);
        register_allocated_insts_vec[i] = std::move(transformed_insts);
    }
    if (enable_logging) {
        report(register_allocated_insts_vec, context, spatial_result.data_cluster_map, register_files_vec, working_dir + "schedule.out");
    }
    
    //* memory allocation
    MemoryAllocator mem_allocator(_target_machine);
    std::vector<std::map<std::string, MemoryLocation>> mem_locations_vec(num_clusters);
    for (int i = 0; i < num_clusters; i++) {
        auto mem_locations = mem_allocator.allocate(register_allocated_insts_vec[i], context);
        mem_locations_vec[i] = mem_locations; 
    }

    //* 
    std::vector<ClusterModel> cluster_models;
    for (int i = 0; i < num_clusters; i++) {
        cluster_models.push_back(ClusterModel(_target_machine, i));
    }
    std::vector<std::map<uint64_t, std::vector<Inst>>> cluster_instructions_map_in_time_series(num_clusters);
    std::vector<uint64_t> cluster_total_cycles(num_clusters);
    for (int i = 0; i < num_clusters; i++) {
        auto result = cluster_models[i].schedule(register_allocated_insts_vec[i], context, mem_locations_vec[i], working_dir, enable_logging);
        cluster_instructions_map_in_time_series[i] = result.time_series_inst_map;
        cluster_total_cycles[i] = result.total_cycles;
    }
    
    if (enable_logging) {
        reportTimeSeries(cluster_instructions_map_in_time_series, context, spatial_result.data_cluster_map, working_dir + "time_series_schedule.out");
        reportMemoryLocation(cluster_instructions_map_in_time_series, context,  spatial_result.data_cluster_map, mem_locations_vec, 8, working_dir + "memory_allocation.out");
        reportInputMemoryLayout(context, mem_locations_vec, working_dir + "input_memory_layout.txt");
    }

    return {cluster_total_cycles};

}

void F1Model::reportTimeSeries(const std::vector<std::map<uint64_t, std::vector<Inst>>>& time_series_insts_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, std::string outpath) {
    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
    const auto& dst_data_cluster_map = data_cluster_map;

    auto printInst = [&context](const Inst& inst, uint32_t allocated_cluster_id, const std::map<std::string, uint32_t>& dst_data_cluster_map) {
        std::stringstream ss;
        auto dag = context.dag;
        ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
        ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << identity;
                if (dst_data_cluster_map.count(identity)) {
                    ss << "(C" << dst_data_cluster_map.at(identity) << ")";
                } else {
                    ss << "(NULL)";
                }
                if (src_id != src_labels.size() - 1) {
                    ss << ", ";
                }
                src_id++;
            }

        }
        ss.str().length() < 100 ? ss << std::string(100 - ss.str().length(), ' ') : ss;
        //* print annotate
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

    std::ofstream fs(outpath);
    for (int i = 0 ; i < num_clusters; i++) {
        fs << "Cluster " << i << ":\n";
        for (const auto& [cycle, insts] : time_series_insts_map[i]) {
            for (const auto& inst : insts) {
                fs << std::format("Cycle {:<5} | ", cycle) << printInst(inst, i, dst_data_cluster_map) << std::endl;
            }
        }
        fs << "\n\n";
    }
}

void F1Model::reportMemoryLocation(const std::vector<std::map<uint64_t, std::vector<Inst>>>& time_series_insts_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, const std::vector<std::map<std::string, MemoryLocation>>& mem_locations_vec, int max_columns, std::string outpath) {
    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
    uint64_t num_banks = _target_machine.micro_arch_cfg.scratchpad_bank;
    uint64_t bank_capacity = _target_machine.micro_arch_cfg.scratchpad_size / num_banks;
    const auto& dst_data_cluster_map = data_cluster_map;

    std::ofstream fs(outpath);

    // 1. Bank Utilization
    // Merge all memory locations into a single map to get a global view and avoid double-counting.
    std::map<std::string, MemoryLocation> global_mem_locations;
    for (const auto& cluster_locs : mem_locations_vec) {
        for (const auto& [label, loc] : cluster_locs) {
            global_mem_locations.try_emplace(label, loc);
        }
    }

    std::vector<uint64_t> bank_usage(num_banks, 0);
    for (const auto& [label, loc] : global_mem_locations) {
        bank_usage[loc.bank_id] += loc.size;
    }

    fs << "================ Bank Utilization ================\n";
    for (int i = 0; i < num_banks; i += max_columns) {
        std::stringstream row1, row2, row3;
        row1 << std::format("{:<15}", "Bank ID:");
        row2 << std::format("{:<15}", "Usage/Cap:");
        row3 << std::format("{:<15}", "Utilization:");

        for (int j = i; j < i + max_columns && j < num_banks; j++) {
            row1 << std::format("{:<15}", std::to_string(j));
            double usage_mb = (double)bank_usage[j] / (1024.0 * 1024.0);
            double cap_mb = (double)bank_capacity / (1024.0 * 1024.0);
            row2 << std::format("{:<15}", std::format("{:.2f}/{} MB", usage_mb, (int)cap_mb));
            double pct = bank_capacity > 0 ? ((double)bank_usage[j] / (double)bank_capacity) * 100.0 : 0.0;
            row3 << std::format("{:<15}", std::format("{:.2f}%", pct));
        }
        fs << row1.str() << "\n" << row2.str() << "\n" << row3.str() << "\n\n";
    }

    // 2. Bank Conflicts
    fs << "================ Bank Conflicts ================\n";

    auto printInst = [&context](const Inst& inst, uint32_t allocated_cluster_id, const std::map<std::string, uint32_t>& dst_data_cluster_map) {
        std::stringstream ss;
        auto dag = context.dag;
        ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
        ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << identity;
                if (dst_data_cluster_map.count(identity)) {
                    ss << "(C" << dst_data_cluster_map.at(identity) << ")";
                } else {
                    ss << "(NULL)";
                }
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

    struct BankAccess {
        uint32_t cluster_id;
        uint64_t start_cycle;
        uint64_t end_cycle;
        Inst inst;
    };
    std::map<uint32_t, std::vector<BankAccess>> bank_accesses;
    uint64_t reg_size = _target_machine.micro_arch_cfg.vector_register_size;

    for (int cluster_id = 0; cluster_id < time_series_insts_map.size(); cluster_id++) {
        for (const auto& [cycle, insts] : time_series_insts_map[cluster_id]) {
            for (const auto& inst : insts) {
                bool is_mem_access = false;
                uint32_t bank_id = 0;

                if (inst.getId() == InstId::Load) {
                    if (!inst.getSrcLabels().empty()) {
                        std::string identity = inst.getSrcLabels()[0].second;
                        auto mem_it = mem_locations_vec[cluster_id].find(identity);
                        if (mem_it != mem_locations_vec[cluster_id].end()) {
                            bank_id = mem_it->second.bank_id;
                            is_mem_access = true;
                        }
                    }
                } else if (inst.getId() == InstId::Store) {
                    std::string identity = inst.getDstLabel().second;
                    auto mem_it = mem_locations_vec[cluster_id].find(identity);
                    if (mem_it != mem_locations_vec[cluster_id].end()) {
                        bank_id = mem_it->second.bank_id;
                        is_mem_access = true;
                    }
                }

                if (is_mem_access) {
                    uint64_t register_span = 1;
                    auto dst_node = context.dag->getNode(inst.getDstLabel().first);
                    if (auto reg_node = std::dynamic_pointer_cast<RegisterNode>(dst_node)) {
                        auto span = reg_node->getRegSpan();
                        register_span = span.second - span.first + 1;
                    } else {
                        register_span = (dst_node->dataSize() + reg_size - 1) / reg_size;
                        if (register_span == 0) register_span = 1;
                    }

                    uint64_t end_cycle = cycle + register_span - 1;
                    bank_accesses[bank_id].push_back({(uint32_t)cluster_id, cycle, end_cycle, inst});
                }
            }
        }
    }

    int conflict_count = 0;
    for (auto& [bank_id, accesses] : bank_accesses) {
        std::sort(accesses.begin(), accesses.end(), [](const BankAccess& a, const BankAccess& b) {
            if (a.start_cycle != b.start_cycle)
                return a.start_cycle < b.start_cycle;
            return a.end_cycle < b.end_cycle;
        });

        for (size_t i = 0; i < accesses.size(); ++i) {
            std::vector<BankAccess> overlapping;
            overlapping.push_back(accesses[i]);
            uint64_t current_overlap_end = accesses[i].end_cycle;

            size_t j = i + 1;
            while (j < accesses.size() && accesses[j].start_cycle <= current_overlap_end) {
                overlapping.push_back(accesses[j]);
                current_overlap_end = std::max(current_overlap_end, accesses[j].end_cycle);
                j++;
            }

            if (overlapping.size() > 1) {
                conflict_count++;
                fs << std::format("Bank {} Conflict! Cycles {}-{} ({} overlapping accesses)\n", 
                                  bank_id, accesses[i].start_cycle, current_overlap_end, overlapping.size());
                for (const auto& acc : overlapping) {
                    fs << std::format("    [C{:<2} Cyc {:<5}-{:<5}] ", acc.cluster_id, acc.start_cycle, acc.end_cycle)
                       << printInst(acc.inst, acc.cluster_id, dst_data_cluster_map) << "\n";
                }
                i = j - 1; // Fast-forward past this group of grouped conflicts
            }
        }
    }

    if (conflict_count == 0) {
        fs << "No bank conflicts detected.\n";
    } else {
        fs << "\nTotal bank conflicts: " << conflict_count << "\n";
    }
}

void F1Model::reportInputMemoryLayout(F1ModelContext& context, const std::vector<std::map<std::string, MemoryLocation>>& mem_locations_vec, std::string outpath) {
    std::ofstream fs(outpath);
    if (!fs.is_open()) {
        std::cerr << "Error: Could not open file for writing: " << outpath << std::endl;
        return;
    }

    fs << "================ Input Memory Layout by Bank ================\n";

    // 1. Merge per-cluster memory locations into a single global map to get a unique view of all allocations.
    std::map<std::string, MemoryLocation> global_mem_locations;
    for (const auto& cluster_locs : mem_locations_vec) {
        for (const auto& [label, loc] : cluster_locs) {
            global_mem_locations.try_emplace(label, loc);
        }
    }

    // 2. Filter for input data nodes (ct, c2, ksk) and group them by their allocated bank.
    uint64_t num_banks = _target_machine.micro_arch_cfg.scratchpad_bank;
    std::vector<std::vector<std::pair<std::string, MemoryLocation>>> bank_contents(num_banks);

    for (int dt_int = 0; dt_int <= static_cast<int>(DataType::UINT512); ++dt_int) {
        auto& nodes = context.dag->getDataNodeByDataType(static_cast<DataType>(dt_int));
        for (const auto& node : nodes) {
            if (!node) continue;
            std::string sym_name = node->getSymbolicName();
            if (sym_name == "ct" || sym_name == "c2" || sym_name == "ksk") {
                std::vector<NodePtr> coeff_nodes;
                DataType dt = static_cast<DataType>(dt_int);

                if (dt == DataType::KeySwitchKey) {
                    auto ksk_node = std::static_pointer_cast<KeySwitchKeyNode>(node);
                    for (uint32_t i = 0; i < ksk_node->L(); ++i) {
                        for (uint32_t j = 0; j < 2; ++j) {
                            for (uint32_t k = 0; k < ksk_node->at(i)->at(j)->getModulusSize(); ++k) {
                                coeff_nodes.push_back(ksk_node->at(i, j, k));
                            }
                        }
                    }
                } else if (dt == DataType::Ciphertext) {
                    auto ct_node = std::static_pointer_cast<CipherTextNode>(node);
                    for (uint32_t i = 0; i < 2; ++i) {
                        for (uint32_t j = 0; j < ct_node->at(i)->getModulusSize(); ++j) {
                            coeff_nodes.push_back(ct_node->at(i, j));
                        }
                    }
                } else if (dt == DataType::PublicKey) {
                    auto pk_node = std::static_pointer_cast<PublicKeyNode>(node);
                    for (uint32_t i = 0; i < 2; ++i) {
                        for (uint32_t j = 0; j < pk_node->at(i)->getModulusSize(); ++j) {
                            coeff_nodes.push_back(pk_node->at(i, j));
                        }
                    }
                } else if (dt == DataType::RnsPoly) {
                    auto rns_node = std::static_pointer_cast<RnsPolyNode>(node);
                    for (uint32_t i = 0; i < rns_node->getModulusSize(); ++i) {
                        coeff_nodes.push_back(rns_node->at(i));
                    }
                } else if (dt == DataType::CoeffVec) {
                    coeff_nodes.push_back(node);
                }

                for (const auto& coeff_node : coeff_nodes) {
                    std::string identity = coeff_node->getIdentity();
                    auto it = global_mem_locations.find(identity);
                    if (it != global_mem_locations.end()) {
                        auto loc = it->second;
                        if (loc.bank_id < num_banks) {
                            bank_contents[loc.bank_id].push_back({coeff_node->getSymbolicName(), loc});
                        }
                    }
                }
            }
        }
    }

    // 3. For each bank, sort its contents by address and write to the file.
    for (uint32_t i = 0; i < num_banks; ++i) {
        fs << "\n--- Bank " << i << " ---\n";
        
        // Sort by address for a clean layout visualization.
        std::sort(bank_contents[i].begin(), bank_contents[i].end(), 
            [](const auto& a, const auto& b) {
                return a.second.address < b.second.address;
            });

        if (bank_contents[i].empty()) {
            fs << "(empty)\n";
            continue;
        }

        for (const auto& [label, loc] : bank_contents[i]) {
            fs << std::format("[ {:<15} | Addr: {:<8} | Size: {:<8} ]\n", 
                              label, loc.address, loc.size);
        }
    }

    fs.close();
    std::cout << "Input memory layout visualization written to " << outpath << std::endl;
}

void F1Model::printInstList(const std::vector<Inst>& insts, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, std::string outpath) {
    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
    const auto& dst_data_cluster_map = data_cluster_map;
    //* generate instruction sequence
    std::map<uint64_t, uint64_t> data_mv_count_between_clusters_map;
    std::map<uint64_t, std::vector<std::string>> cluster_data_mv_map;
    std::vector<std::vector<uint64_t>> data_mv_count_map(num_clusters, std::vector<uint64_t>(num_clusters, 0)); //* data_mv_count_map[src_cluster][dst_cluster]
    std::vector<uint64_t> data_mv_from_scratchpad_count(num_clusters, 0);

    auto printInst = [&context](const Inst& inst, uint32_t allocated_cluster_id, const std::map<std::string, uint32_t>& dst_data_cluster_map) {
        std::stringstream ss;
        auto dag = context.dag;
        ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
        ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
        {
            auto& src_labels = inst.getSrcLabels();
            int src_id = 0;
            for (auto& [node_id, identity] : src_labels) {
                ss << identity;
                if (dst_data_cluster_map.count(identity)) {
                    ss << "(C" << dst_data_cluster_map.at(identity) << ")";
                } else {
                    ss << "(NULL)";
                }
                if (src_id != src_labels.size() - 1) {
                    ss << ", ";
                }
                src_id++;
            }

        }
        ss.str().length() < 100 ? ss << std::string(100 - ss.str().length(), ' ') : ss;
        //* print annotate
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


    std::ofstream fs(outpath);
    for (auto& inst : insts) {
        fs << printInst(inst, 0, dst_data_cluster_map) << std::endl;
    } 
}


std::string F1Model::analyzeRegisterUtilization(const std::vector<RegisterFile>& reg_files, int n) {
    int num_clusters = _target_machine.micro_arch_cfg.num_clusters;
    std::stringstream final_result;

    for (int i = 0; i < num_clusters; i += n) {
        std::stringstream row1, row2, row3;
        
        row1 << std::format("{:<15}", "Cluster ID:");
        row2 << std::format("{:<15}", "Usage/Cap:");
        row3 << std::format("{:<15}", "Histogram:");

        for (int j = i; j < i + n && j < num_clusters; j++) {
            const auto& reg_file = reg_files[j];
            uint64_t peak_usage = reg_file.getPeakAllocatedSize();
            uint64_t cap = reg_file.getTotalRegNum();
            
            row1 << std::format("{:<15}", std::to_string(j));
            row2 << std::format("{:<15}", std::to_string(peak_usage) + "/" + std::to_string(cap));
            
            int bars = cap > 0 ? (peak_usage * 10) / cap : 0;
            // std::string histo = std::string(bars, '*') + std::string(10 - bars, '-');
            // row3 << std::format("{:<15}", histo);
        }
        
        final_result << row1.str() << "\n" << row2.str() << "\n" << row3.str() << "\n";
        if (i + n < num_clusters) {
            final_result << "\n"; // Optional line break between cluster groups
        }
    }
    
    return final_result.str();
}

}