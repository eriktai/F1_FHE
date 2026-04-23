#include "F1Model.hpp"
#include "DAG.hpp"
#include "RegisterNode.hpp"
#include "ClusterModel.hpp"
#include <stdexcept>


namespace f1 {

F1Model::F1Model(F1TargetMachine target_machine)
    : _target_machine(std::move(target_machine))
    {} 

uint32_t SpatialScheduler::clusterAllocation(const Inst& inst, const std::map<uint32_t, uint32_t>& candidate_cluster_counts, const std::map<uint32_t, std::vector<Inst>>& cluster_instructions_map, int num_clusters) {
    std::vector<uint32_t> primary_candidates;

    // Priority 1: Data Temporal Locality
    if (candidate_cluster_counts.empty()) {
        for (int i = 0; i < num_clusters; ++i) {
            primary_candidates.push_back(i);
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
                primary_candidates.push_back(cid);
            }
        }
    }

    if (primary_candidates.size() == 1) {
        return primary_candidates[0];
    }

    // Priority 2: Pipeline Load Balancing
    InstId target_id = inst.getId();
    uint32_t best_cluster = primary_candidates[0];
    uint32_t min_load = UINT32_MAX;
    uint32_t min_total_load = UINT32_MAX;

    auto getPipelineType = [](InstId id) {
        if (id == InstId::NTT || id == InstId::INTT) return 1;
        if (id == InstId::ModMul) return 2;
        if (id == InstId::ModAdd) return 3;
        return 0; // Miscellaneous (e.g., Load)
    };
    
    int target_pipeline = getPipelineType(target_id);

    for (uint32_t cid : primary_candidates) {
        uint32_t current_pipeline_load = 0;
        uint32_t total_load = 0;
        
        auto it = cluster_instructions_map.find(cid);
        if (it != cluster_instructions_map.end()) {
            total_load = it->second.size();
            for (const auto& scheduled_inst : it->second) {
                if (getPipelineType(scheduled_inst.getId()) == target_pipeline) {
                    current_pipeline_load++;
                }
            }
        }

        if (current_pipeline_load < min_load) {
            min_load = current_pipeline_load;
            best_cluster = cid;
            min_total_load = total_load;
        } else if (current_pipeline_load == min_load) {
            // Tie-breaker: If pipeline loads are equal, fall back to the overall cluster load
            if (total_load < min_total_load) {
                min_total_load = total_load;
                best_cluster = cid;
            }
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
        uint32_t allocated_cluster_id = clusterAllocation(inst, candidate_cluster_counts, cluster_instructions_map, num_clusters);

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
    assert(num_clusters == cluster_instructions_map.size());
    for (int i = 0 ; i < num_clusters; i++) {
        fs << "Cluster " << i << ":\n";
        uint64_t num_data_movement_between_clusters = 0;
        for (auto& inst : cluster_instructions_map[i]) {
            fs << printInst(inst, i, dst_data_cluster_map) << std::endl;
            
            if (inst.getId() == InstId::Load) {
                data_mv_from_scratchpad_count[i]++;
            } else {
                for (auto& [node_id, identity] : inst.getSrcLabels()) {
                    if (dst_data_cluster_map.count(identity) ) {
                        if (dst_data_cluster_map.at(identity) != i) {
                            num_data_movement_between_clusters++;
                        }
                        data_mv_count_map[dst_data_cluster_map.at(identity)][i]++;
                        // std::string mv_key = std::format("C{}->C{}", dst_data_cluster_map[identity], i);
                    }
                } 
            }
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

    for (auto inst : instructions) {
        auto cur_seq_id = inst.getInstSeqId();

        // ------------------------------------------------------------------
        // 1. Liveness Expiration
        // Scan all active registers and release those whose lifespan has ended.
        // ------------------------------------------------------------------
        for (auto it = register_nodes_map.begin(); it != register_nodes_map.end(); ) {
            auto& [effective_id, reg_node] = *it;

            auto liveness = liveness_map.at(effective_id);
            if (cur_seq_id < liveness.first) {
                throw std::runtime_error("Encountered a register allocated before its liveness");
            }
            if (cur_seq_id > liveness.second) {
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

    for (auto inst : instructions) {
        auto inst_seq_id = inst.getInstSeqId();
        //* check dst data liveness
        auto& [dst_node_id, dst_identity] = inst.getDstLabel();
        auto effective_id = context.dag->getNode(dst_node_id)->getEffectiveId();
        if (liveness_map.find(effective_id) == liveness_map.end()) {
            liveness_map[effective_id] = {inst_seq_id, inst_seq_id};
        } else {
            liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, inst_seq_id); 
            liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, inst_seq_id);
        }

        for (const auto& [src_node_id, src_identity] : inst.getSrcLabels()) {
            auto effective_id = context.dag->getNode(src_node_id)->getEffectiveId();
            if (liveness_map.find(effective_id) == liveness_map.end()) {
                liveness_map[effective_id] = {inst_seq_id, inst_seq_id};
            } else {
                liveness_map[effective_id].first = std::min(liveness_map[effective_id].first, inst_seq_id); 
                liveness_map[effective_id].second = std::max(liveness_map[effective_id].second, inst_seq_id);
            }
        }        
    }

    return liveness_map;

}

void F1Model::compile(DAG* dag, CipherTextNodePtr root_node) {
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
        
    SpatialScheduler spatial_scheduler;
    auto spatial_result = spatial_scheduler.schedule(instructions, context);

    {

        std::vector<std::vector<Inst>> instsList(num_clusters, std::vector<Inst>{});
        for (int i = 0; i < num_clusters; i++) {
            instsList[i] = spatial_result.cluster_instructions_map[i];
        }
        report(instsList, context, spatial_result.data_cluster_map, register_files_vec, "schedule_before_reg_alloc.out");
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
    report(register_allocated_insts_vec, context, spatial_result.data_cluster_map, register_files_vec);
    
    //* 
    std::vector<ClusterModel> cluster_models;
    for (int i = 0; i < num_clusters; i++) {
        cluster_models.push_back(ClusterModel(_target_machine, i));
    }
    std::vector<std::map<uint64_t, std::vector<Inst>>> cluster_instructions_map_in_time_series(num_clusters);
    for (int i = 0; i < num_clusters; i++) {
        auto res = cluster_models[i].schedule(register_allocated_insts_vec[i], context);
        cluster_instructions_map_in_time_series[i] = res;
    }
    //* memory allocation

    reportTimeSeries(cluster_instructions_map_in_time_series, context, spatial_result.data_cluster_map);

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
            std::string histo = std::string(bars, '*') + std::string(10 - bars, '-');
            row3 << std::format("{:<15}", histo);
        }
        
        final_result << row1.str() << "\n" << row2.str() << "\n" << row3.str() << "\n";
        if (i + n < num_clusters) {
            final_result << "\n"; // Optional line break between cluster groups
        }
    }
    
    return final_result.str();
}

}