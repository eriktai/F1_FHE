#ifndef __MODEL_HH__
#define __MODEL_HH__


#include "Inst.hpp"
#include "PolynomialRing.hpp"
#include "CodeGen.hpp"
#include "ModelContext.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <optional>

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
    uint32_t num_mshr;

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
public:
    using Span = std::pair<uint64_t, uint64_t>;

private:
    std::vector<Span> free_list;
    std::unordered_map<uint64_t, Span> allocation_map;
    int allocated_size;
    int capacity;
    int peak_allocated_size;
public:

    BufferAllocator(uint64_t size) {
        free_list.push_back({0, size-1});
        capacity = size;
        allocated_size = 0;
        peak_allocated_size = 0;
    }

    uint64_t getAllocatedSize() const {
        return allocated_size;
    }

    uint64_t getPeakAllocatedSize() const {
        return peak_allocated_size;
    }

    uint64_t getCapacity() const {
        return capacity;
    }

    std::optional<Span> getAllocationOpt(uint64_t id) {
        if (allocation_map.count(id)) {
            return allocation_map[id];
        } else {
            return std::nullopt;
        }
    }
    

    std::pair<uint64_t, uint64_t> getAllocation(uint64_t id) {
        if (allocation_map.count(id)) {
            return allocation_map[id];
        } else {
            throw std::runtime_error("No allocation found for id " + std::to_string(id));
        }
    }

    bool canAllocate(uint64_t size) {
        for (auto it = free_list.begin(); it != free_list.end(); ++it) {
            uint64_t start = it->first;
            uint64_t end = it->second;
            if (end - start + 1 >= size) {
                return true;
            }
        }
        return false;
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
                allocated_size += size;
                if (allocated_size > peak_allocated_size) {
                    peak_allocated_size = allocated_size;
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
            allocated_size -= (end - start + 1);
            // Add the released block back to free list
            insertFreeBlock({start, end});
        } else {
            throw std::runtime_error("No allocation found for id " + std::to_string(id));
        }
    }

private:
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
    : _allocator(num_registers * register_size) 
    , _num_registers(num_registers)
    , _register_size(register_size)
    {}

    uint32_t getRegSize() const {
        return _register_size;
    }

    bool canAllocate(uint64_t size) {
        uint32_t register_num = (size + _register_size - 1) / _register_size;
        if (register_num > _num_registers) {
            return false;
        }
        return _allocator.canAllocate(register_num);
    }

    uint64_t getAllocatedNum() const {
        return _allocator.getAllocatedSize() / _register_size;
    }

    uint64_t getAllocatedSize() const {
        return _allocator.getAllocatedSize(); 
    }

    uint64_t getPeakAllocatedNum() const {
        return _allocator.getPeakAllocatedSize() / _register_size;
    }


    uint64_t getPeakAllocatedSize() const {
        return _allocator.getPeakAllocatedSize();
    }

    uint64_t getTotalRegNum() const {
        return _num_registers;
    }

    uint64_t getCapacity() const {
        return _allocator.getCapacity();
    }

    void releaseAllocated(uint64_t id) {
        _allocator.releaseAllocated(id);
    }

    BufferAllocator::Span allocate(uint64_t id, uint64_t size) {
        uint32_t register_num = (size + _register_size - 1) / _register_size;
        if (register_num > _num_registers) {
            throw std::runtime_error("Requested register size exceeds the total register file size");
        }
        return _allocator.allocate(id, register_num);
    }
    

    std::pair<uint64_t, uint64_t> getAllocatedRegs(uint64_t id) {
        return _allocator.getAllocation(id);
    }

    std::optional<std::pair<uint64_t, uint64_t>> getAllocatedRegsOpt(uint64_t id) {
        return _allocator.getAllocationOpt(id);
    }

private:
    BufferAllocator _allocator;
    uint64_t _num_registers;
    uint64_t _register_size;

};

class SRAMModel {
    using Span = std::pair<uint64_t, uint64_t>;
    class Bank : public BufferAllocator {
    public:
        Bank(uint64_t id, uint64_t size, uint64_t bandwidth)
        : BufferAllocator(size)
        , _id(id)
        , _size(size)
        , _bandwidth(bandwidth)
        {}

        std::pair<uint64_t, uint64_t> allocate(uint64_t id, uint64_t size) {
            return this->allocate(id, size);
        }

        void release(uint64_t id) {
            this->releaseAllocated(id);
        }

    
    private:
        uint64_t _id;
        uint64_t _size;
        uint64_t _bandwidth;

        // BufferAllocator bank_buffer;
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

using DataLivenessMap = std::map<uint64_t, std::pair<uint64_t, uint64_t>>;

struct F1TargetMachine {
    MicroArchConfig micro_arch_cfg;
    SoftwareConfig software_cfg;

    SRAMModel scratchpad;
    RegisterFile register_file;
    std::vector<ComputeClusterModel> compute_clusters;

    F1TargetMachine(MicroArchConfig micro_arch_config, SoftwareConfig software_config)
        : micro_arch_cfg(micro_arch_config)
        , software_cfg(software_config)
        , scratchpad(micro_arch_config.scratchpad_size, micro_arch_config.scratchpad_bank, micro_arch_config.scratchpad_bandwidth)
        , register_file(micro_arch_config.vector_register_num, micro_arch_config.vector_register_size)
        , compute_clusters(micro_arch_config.num_clusters, 
            ComputeClusterModel(micro_arch_config.cluster_config.num_NTT, micro_arch_config.cluster_config.num_MM, micro_arch_config.cluster_config.num_MA, 
                micro_arch_config.cluster_config.ntt_config, micro_arch_config.cluster_config.mm_config, micro_arch_config.cluster_config.ma_config))
    {}
};

struct TemporalSchedulerResult {
    std::map<uint64_t, std::vector<Inst>> time_series_inst_map;
    uint64_t total_cycles;
};

struct F1ModelResult {
    std::vector<uint64_t> cluster_total_cycles;
};

// struct F1ModelContext {
// public:

//     //* DAG graph
//     DAG* dag;
//     const F1TargetMachine* target_machine;
 
// };

struct SpatialMappingResult {
    std::map<uint32_t, std::vector<Inst>> cluster_instructions_map;
    std::map<std::string, uint32_t> data_cluster_map;
};

class SpatialScheduler {
    RoundRobinScheduler rr_sched;
public:
    SpatialScheduler(int n)
    : rr_sched(n) {}

    uint32_t clusterAllocation(const Inst& inst, const std::map<uint32_t, uint32_t>& candidate_cluster_counts, const std::map<uint32_t, std::vector<Inst>>& cluster_instructions_map, int num_clusters);

    SpatialMappingResult schedule(const std::vector<Inst>& instructions, F1ModelContext& context);

private:
    std::vector<uint32_t> evaluateDataTemporalLocality(const std::map<uint32_t, uint32_t>& candidate_cluster_counts, int num_clusters);
    std::vector<uint32_t> evaluatePipelineLoad(const Inst& inst, const std::vector<uint32_t>& candidates, const std::map<uint32_t, std::vector<Inst>>& cluster_instructions_map);
};

struct MemoryLocation;

class LivenessAnalyzer {
public:
    DataLivenessMap compute(const std::vector<Inst>& instructions, F1ModelContext& context);
};

class RegisterAllocator {
public:
    std::vector<Inst> allocate(int cluster_id, const std::vector<Inst>& instructions, const DataLivenessMap& liveness_map, std::map<std::string, uint32_t>& data_cluster_map, RegisterFile& reg_file, F1ModelContext& context);
};

class F1Model {
public:

    F1Model(F1TargetMachine target_machine);

    F1ModelResult compile(DAG* dag, CipherTextNodePtr root_node, std::string working_dir = "", bool enable_logging = false);

    void report(const std::vector<std::vector<Inst>>& insts_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, const std::vector<RegisterFile>& reg_files, std::string outpath = "schedule.out");

    void reportTimeSeries(const std::vector<std::map<uint64_t, std::vector<Inst>>>& time_series_insts_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, std::string outpath = "time_series_schedule.out");

    void reportMemoryLocation(const std::vector<std::map<uint64_t, std::vector<Inst>>>& time_series_insts_map, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, const std::vector<std::map<std::string, MemoryLocation>>& mem_locations_vec, int max_columns = 8, std::string outpath = "memory_allocation.out");

    void reportInputMemoryLayout(F1ModelContext& context, const std::vector<std::map<std::string, MemoryLocation>>& mem_locations_vec, std::string outpath);

    void printInstList(const std::vector<Inst>& insts, F1ModelContext& context, const std::map<std::string, uint32_t>& data_cluster_map, std::string outputpath);
         
    std::string analyzeRegisterUtilization(const std::vector<RegisterFile>& reg_files, int n = 8);

private:

    F1TargetMachine _target_machine;
};
class TemporalScheduler {

public:

    TemporalScheduler(const F1TargetMachine& target_machine);



private:
    const F1TargetMachine& _target_machine;

};


}

#endif