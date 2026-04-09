#ifndef __MODEL_HH__
#define __MODEL_HH__


#include "Inst.hpp"
#include "PolynomialRing.hpp"
#include "CodeGen.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <unordered_map>

namespace f1 {


struct ALUConfig {
    uint32_t latency;
    uint32_t num_src; //* number of source operands
    uint32_t num_dst; //* number of destination operands
    uint32_t word_size; //* the word size of the ALU, e.g., 64 bits
    uint32_t lane; //* number of parallel execution lane, total bandwidth = bandwidth * lane
    bool fully_pipeline; //* whether the ALU is fully pipelined, i.e., can accept new input every cycle
};

struct NTTConfig : public ALUConfig {
    uint32_t n_points_ntt;

    NTTConfig(uint32_t np, uint32_t word_size, bool fully_pipeline)
    : ALUConfig{.latency = 0, 
                .num_src = np, 
                .num_dst = np, 
                .word_size = word_size, 
                .lane = 1, 
                .fully_pipeline = fully_pipeline} 
    , n_points_ntt(np)
    {}

};


struct ClusterConfig {
    uint32_t num_NTT;
    uint32_t num_MM;
    uint32_t num_MA;

    NTTConfig ntt_config;
    ALUConfig mm_config;
    ALUConfig ma_config;
};

struct SoftwareConfig {
    uint64_t degree;
    uint64_t modulus_size;
};

struct MicroArchConfig {
    uint64_t scratchpad_size;
    uint64_t scratchpad_bank;
    uint64_t scratchpad_bandwidth;
    uint64_t vector_register_size;
    uint64_t vector_register_num;
    uint64_t num_clusters;
    ClusterConfig cluster_config;
};

class Scratchpad {
public:
    Scratchpad(uint64_t size)
    : _size(size)
    , free_list_chain{{0, size-1}}
    {}

private:
    uint64_t _size;
    std::vector<std::vector<uint64_t>> free_list_chain;
    std::vector<std::vector<uint64_t>> allocated_list_chain;
};


class BufferAllocator {

    using Span = std::pair<uint64_t, uint64_t>;

    std::vector<Span> free_list;
    std::unordered_map<uint64_t, Span> allocation_map;
public:

    BufferAllocator(uint64_t size) {
        free_list.push_back({0, size-1});
    }

    std::pair<uint64_t, uint64_t> allocate(uint64_t id, uint64_t size) {
        for (auto it = free_list.begin(); it != free_list.end(); ++it) {
            uint64_t start = it->first;
            uint64_t end = it->second;
            if (end - start + 1 >= size) {
                // Allocate this block
                // allocated_list.push_back({id, start, start + size - 1});
                allocation_map[id] = {start, start + size - 1};
                // Update the free block
                if (end - start + 1 == size) {
                    free_list.erase(it);
                } else {
                    it->first += size;
                }
                return allocation_map[id];
            }
        }
        throw std::runtime_error("Not enough memory to allocate for id " + std::to_string(id));
    }


    void releaseAllocated(uint64_t id) {
        auto it = allocation_map.find(id);
        if (it != allocation_map.end()) {
            uint64_t start = it->second.first;
            uint64_t end = it->second.second; 
            allocation_map.erase(it);
            // Add the released block back to free list
            insertFreeBlock({start, end});
        } else {
            throw std::runtime_error("No allocation found for id " + std::to_string(id));
        }
    }

    bool isOverlapped(Span s1, Span s2) {
        if (s1.first > s2.first) {
            swap(s1, s2);
        }
        return s1.second >= s2.first;
    }

    void insertFreeBlock(Span new_block) {

        int l = 0; 
        int r = free_list.size() - 1;

        uint64_t start = new_block.first;
        uint64_t end = new_block.second;

        auto isBefore = [&](int idx) {
            return free_list[idx].second < start;
        };


        if (!isBefore(l)) {
            if (isOverlapped(new_block, free_list[l])) {
                throw std::runtime_error("Overlapping free blocks after release");
            }
            free_list.insert(free_list.begin(), new_block);
            return;
        }

        if (isBefore(r)) {
            if (isOverlapped(new_block, free_list[r])) {
                throw std::runtime_error("Overlapping free blocks after release");
            }
            free_list.push_back(new_block);
            return;
        }

        //* binary search the position to insert
        while (r - l > 1) {
            int mid = l + (r - l) / 2;
            if (isBefore(mid)) {
                l = mid;
            } else {
                r = mid;
            }
        }
        
        if (isOverlapped(new_block, free_list[l]) || isOverlapped(new_block, free_list[r])) {
            throw std::runtime_error("Overlapping free blocks after release");
        }

        //* check if left block can be merged
        if (free_list[l].second + 1 == start && free_list[r].first - 1 == end) {
            free_list[l].second = free_list[r].second;
            free_list.erase(free_list.begin() + r);
        } else if (free_list[l].second + 1 == start) {
            free_list[l].second = end;
        } else if (free_list[r].first - 1 == end) {
            free_list[r].first = start;
        } else {
            free_list.insert(free_list.begin() + r, new_block);
        }
    }


};

class RegisterFile {
public:
    RegisterFile(uint64_t num_registers, uint64_t register_size)
    : _num_registers(num_registers)
    , _register_size(register_size)
    , register_buffers(num_registers)
    {}

    
    std::pair<uint64_t, uint64_t> allocate(uint64_t id, uint64_t size) {
        uint64_t n_registers = size / _register_size + ((size % _register_size) ? 1 : 0); 
        return register_buffers.allocate(id, n_registers); 
    }

    
private:
    uint64_t _num_registers;
    uint64_t _register_size;

    std::vector<uint64_t> registers;

    BufferAllocator register_buffers;

};

class SRAMModel {
    using Span = std::pair<uint64_t, uint64_t>;
    class Bank {
    public:
        Bank(uint64_t id, uint64_t size, uint64_t bandwidth)
        : _id(id)
        , _size(size)
        , _bandwidth(bandwidth)
        , bank_buffer(size)
        {}

        std::pair<uint64_t, uint64_t> allocate(uint64_t id, uint64_t size) {
            return bank_buffer.allocate(id, size);
        }

        void release(uint64_t id) {
            bank_buffer.releaseAllocated(id);
        }

    
    private:
        uint64_t _id;
        uint64_t _size;
        uint64_t _bandwidth;

        BufferAllocator bank_buffer;
    };

public:
    SRAMModel(uint64_t size, uint64_t bank, uint64_t bandwidth)
    : _size(size)
    , _bank(bank)
    , _bandwidth(bandwidth)
    , banks()
    {
        uint64_t bank_size = size / bank;
        for (uint64_t i = 0; i < bank; i++) {
            banks.emplace_back(i, bank_size, bandwidth);
        }
    }

private:
    uint64_t _size;
    uint64_t _bank;
    uint64_t _bandwidth;
    std::vector<Bank> banks;
};

class ALUModel {
public:
    ALUModel(ALUConfig config)
    : _config(config)
    {}

    virtual void execute() = 0;

private:
    ALUConfig _config;
};

class ComputeClusterModel {

public:
    ComputeClusterModel(uint64_t num_NTT, uint64_t num_MM, uint64_t num_MA, 
                        NTTConfig ntt_config, ALUConfig mm_config, ALUConfig ma_config)
    : _num_NTT(num_NTT)
    , _num_MM(num_MM)
    , _num_MA(num_MA)
    , _ntt_config(ntt_config)
    , _mm_config(mm_config)
    , _ma_config(ma_config)
    {}


private:
    uint64_t _num_NTT;
    uint64_t _num_MM;
    uint64_t _num_MA;

    NTTConfig _ntt_config;
    ALUConfig _mm_config;
    ALUConfig _ma_config;
    
    //* liveness analysis

};

class RoundRobinScheduler {
public:
    RoundRobinScheduler(int n)
    : _n(n)
    , _current(0)
    {}

    uint32_t schedule() {
        uint32_t cluster_id = _current;
        _current = (_current + 1) % _n;
        return cluster_id;
    }


private:
    int _n;
    int _current;
};

class F1Model {
public:

    F1Model(MicroArchConfig micro_arch_config, SoftwareConfig software_config)
    : _micro_arch_cfg(micro_arch_config)
    , _software_cfg(software_config)
    , scratchpad(micro_arch_config.scratchpad_size, micro_arch_config.scratchpad_bank, micro_arch_config.scratchpad_bandwidth)
    , register_file(micro_arch_config.vector_register_num, micro_arch_config.vector_register_size)
    , compute_clusters(micro_arch_config.num_clusters, 
                ComputeClusterModel(micro_arch_config.cluster_config.num_NTT, micro_arch_config.cluster_config.num_MM, micro_arch_config.cluster_config.num_MA, 
                    micro_arch_config.cluster_config.ntt_config, micro_arch_config.cluster_config.mm_config, micro_arch_config.cluster_config.ma_config))
    {} 




    void compile(DAG* dag, CipherTextNodePtr root_node) {

        //* 1. Traverse the DAG to get the instruction sequence
        CodeGenerator code_gen(dag);
        //* travel the DAG and generate instruction sequence
        code_gen.travel(root_node);

        auto& instructions = code_gen.getInstructions();


        //* start the spatial mapping
        int num_clusters = _micro_arch_cfg.num_clusters;
        //* cluster_instruction_map: cluster_id -> instructions
        std::map<uint32_t, std::vector<Inst>> cluster_instruction_map;
        //* dstination data generated from which cluster, used for routing
        std::map<std::string, uint32_t> dst_data_cluster_map;        
        //* source data liveness map
        std::map<std::string, uint32_t> src_data_cluster_map; 

        RoundRobinScheduler cluster_rr_scheduler(num_clusters);

        std::ofstream fs("schedule.out");
        
        uint64_t inst_seq_id = 0;

        auto printInst = [&dag](const Inst& inst, uint32_t allocated_cluster_id, std::map<std::string, uint32_t>& dst_data_cluster_map) {
            std::stringstream ss;
            ss << std::format("{:<5}: {:<7}(C{:<2}) ", inst.getInstSeqId(), ToInstStr(inst.getId()), allocated_cluster_id);
            ss << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
            {
                auto& src_labels = inst.getSrcLabels();
                int src_id = 0;
                for (auto& [node_id, identity] : src_labels) {
                    ss << identity;
                    if (dst_data_cluster_map.count(identity)) {
                        ss << "(C" << dst_data_cluster_map[identity] << ")";
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


        for (int i = 0; i < instructions.size(); i++) {

            auto& inst = instructions[i];

            //* check the source data dependency.
            auto& src_labels = inst.getSrcLabels();
            std::map<uint32_t, uint32_t> dst_candidate_clusters;
            for (auto& [node_id, identity] : src_labels) {
                if (dst_data_cluster_map.count(identity)) {
                    dst_candidate_clusters[dst_data_cluster_map[identity]]++;
                }
            }

            
            //* if empty, it means no dependency, we can schedule it to any cluster
            uint32_t allocated_cluster_id;
            if (dst_candidate_clusters.empty()) {
                //* source data haven't been loaded into register
                //* generate a load instruction to load the source data to the cluster, and update the src_data_cluster_map
                allocated_cluster_id = cluster_rr_scheduler.schedule();
                //* generate the load instruction for source data 
            } else {
                //* if not empty, we can schedule it to the cluster that has the most dependencies
                //* because the instruction can only have most 2 source operands, the size of candidate_clusters can be at most 2
                if (dst_candidate_clusters.size() == 1) {
                    allocated_cluster_id = dst_candidate_clusters.begin()->first;
                } else if (dst_candidate_clusters.size() == 2) {
                    //* if there are multiple candidate clusters, we can choose one of them, e.g., the one with less instructions scheduled
                    uint32_t cluster_0 = dst_candidate_clusters.begin()->first;
                    uint32_t cluster_1 = (++dst_candidate_clusters.begin())->first;
                    if (cluster_instruction_map[cluster_0].size() <= cluster_instruction_map[cluster_1].size()) {
                        allocated_cluster_id = cluster_0;
                    } else {
                        allocated_cluster_id = cluster_1;
                    }
                }
            }

            //* generate load instruction for them
            for (auto& [node_id, identity] : src_labels) {
                if (!dst_data_cluster_map.count(identity)) {
                    //* generate load instruction to load the source data to the allocated cluster
                    // fs << std::format("LOAD: {} -> C{}\n", identity, allocated_cluster_id);
                    std::string dst_identity = std::format("C{}::reg[NULL]", allocated_cluster_id);
                    Inst ld_inst = Inst(ToInstId("Load"), {node_id, dst_identity}, {{node_id, identity}});
                    ld_inst.setInstSeqId(inst.getInstSeqId());
                    dst_data_cluster_map[identity] = allocated_cluster_id;
                    fs << printInst(ld_inst, allocated_cluster_id, dst_data_cluster_map) << std::endl;
                    //* allocate register for it
                    cluster_instruction_map[allocated_cluster_id].push_back(ld_inst);
                }
            }

            cluster_instruction_map[allocated_cluster_id].push_back(inst);
            dst_data_cluster_map[inst.getDstLabel().second] = allocated_cluster_id;

            //* print allocated cluster id
            fs << printInst(inst, allocated_cluster_id, dst_data_cluster_map) << std::endl;
            // fs << std::format("{:<5}: {:<7}(C{:<2}) ", inst_seq_id++, ToInstStr(inst.getId()), allocated_cluster_id);
            // fs << inst.getDstLabel().second << "(C" << allocated_cluster_id << ")" << ", ";
            // {
            //     int src_id = 0;
            //     for (auto& [node_id, identity] : src_labels) {
            //         fs << identity;
            //         if (dst_data_cluster_map.count(identity)) {
            //             fs << "(C" << dst_data_cluster_map[identity] << ")";
            //         } else {
            //             fs << "(NULL)";
            //         }
            //         if (src_id != src_labels.size() - 1) {
            //             fs << ", ";
            //         }
            //         src_id++;
            //     }

            // }
            // fs << std::endl;
        }

        //* generate instruction sequence
        std::map<uint64_t, uint64_t> data_mv_count_between_clusters_map;
        std::map<uint64_t, std::vector<std::string>> cluster_data_mv_map;
        std::vector<std::vector<uint64_t>> data_mv_count_map(num_clusters, std::vector<uint64_t>(num_clusters, 0)); //* data_mv_count_map[src_cluster][dst_cluster]
        std::vector<uint64_t> data_mv_from_scratchpad_count(num_clusters, 0);
        for (int i = 0 ; i < num_clusters; i++) {
            fs << "Cluster " << i << ":\n";
            uint64_t num_data_movement_between_clusters = 0;
            for (auto& inst : cluster_instruction_map[i]) {
                fs << printInst(inst, i, dst_data_cluster_map) << std::endl;
                
                if (inst.getId() == InstId::Load) {
                    data_mv_from_scratchpad_count[i]++;
                } else {
                    for (auto& [node_id, identity] : inst.getSrcLabels()) {
                        if (dst_data_cluster_map.count(identity) ) {
                            if (dst_data_cluster_map[identity] != i) {
                                num_data_movement_between_clusters++;
                            }
                            data_mv_count_map[dst_data_cluster_map[identity]][i]++;
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

        uint64_t coeff_vec_size = _software_cfg.degree * 4;

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
            fs << std::format("Cluster {}: {} instructions\n", i, cluster_instruction_map[i].size());
        }
        fs << "\n";




    }

             
private:

     
    MicroArchConfig _micro_arch_cfg;
    SoftwareConfig _software_cfg; 

    //* MicroArch components
    SRAMModel scratchpad;
    RegisterFile register_file;

    std::vector<ComputeClusterModel> compute_clusters;

};


}

#endif