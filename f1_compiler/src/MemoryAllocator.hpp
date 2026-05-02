#pragma once

#include "F1Model.hpp" // For F1TargetMachine
#include "DAG.hpp"
#include <map>
#include <string>
#include <vector>

namespace f1 {

/*
 * =====================================================================================
 * Memory Allocator Design Plan
 * =====================================================================================
 *
 * 1. Core Purpose
 * ----------------
 * The `MemoryAllocator` is a compiler pass responsible for mapping logical data labels
 * (e.g., "ksk[0][1][0]", "c2[3]") from the FHE computation graph to concrete physical
 * memory addresses within the F1 architecture's banked SRAM. Its primary goal is to
 * lay out data in a way that minimizes memory bank conflicts during execution, thereby
 * maximizing memory bandwidth and overall performance.
 *
 * 2. The Bank Conflict Problem
 * ---------------------------
 * The F1 architecture features a multi-banked SRAM. Each bank can service one memory
 * request per cycle. If multiple, concurrent `Load` instructions (e.g., from different
 * compute clusters) attempt to access data located in the SAME memory bank in the
 * SAME cycle, a "bank conflict" occurs. These requests are serialized, introducing
 * stalls and leaving other banks idle.
 *
 * For example, if Cluster 0 needs `data_A` and Cluster 1 needs `data_B` at the same
 * time, and both `data_A` and `data_B` were allocated to Bank 0, one of the clusters
 * must wait, effectively halving the available memory bandwidth.
 *
 * 3. Proposed Allocation Algorithm
 * --------------------------------
 * The allocator will implement a bank-aware strategy to distribute data across the
 * available SRAM banks.
 *
 *   A. Inputs:
 *      - A list of all unique data labels that need to be stored in SRAM. This can be
 *        derived by analyzing the source operands of all instructions in the program.
 *      - The hardware configuration, specifically the `SRAMModel` from the
 *        `F1TargetMachine`, which defines the number of banks and their capacity.
 *
 *   B. Data Structures:
 *      - `std::vector<BufferAllocator> _bank_allocators`: A vector of allocator
 *        instances, one for each SRAM bank, to track memory usage within that bank.
 *      - `std::map<std::string, MemoryLocation> _allocation_map`: The output map that
 *        stores the final physical address for each data label. `MemoryLocation` will
 *        be a struct containing `{uint32_t bank_id, uint64_t address, uint64_t size}`.
 *
 *   C. Allocation Strategy (Heuristics):
 *      - **Bank-Stripping / Round-Robin (Baseline):** This is the simplest and most
 *        effective initial strategy. Data labels are assigned to banks in a cyclic
 *        manner:
 *          - Label 0 -> Bank 0
 *          - Label 1 -> Bank 1
 *          - ...
 *          - Label N -> Bank (N % num_banks)
 *        This naturally distributes data evenly, reducing the probability of random
 *        concurrent accesses hitting the same bank.
 *
 * 4. Analogy to a Traditional Linker
 * ---------------------------------
 * This `MemoryAllocator` acts as a specialized linker for our FHE accelerator.
 *
 *   - **Linker:** A traditional linker takes compiled object files (`.o`) where functions
 *     and global variables are just "symbols" with no fixed address. It uses a "linker
 *     script" that defines the target's memory map (e.g., FLASH at 0x08000000, RAM at
 *     0x20000000). The linker's job is to "resolve" these symbols by assigning each one a
 *     final, concrete address within the appropriate memory region, producing a single
 *     executable file.
 *
 *   - **Our Allocator:** Our `MemoryAllocator` performs the same role.
 *     - **Symbols:** The logical data labels (e.g., "c2[0]").
 *     - **Linker Script:** The `F1TargetMachine` and its `SRAMModel` define our memory map
 *       (N banks of M MB each).
 *     - **Resolving Symbols:** The `allocate` function assigns each label a physical
 *       `(bank_id, address)` pair.
 *
 * The key difference is that our allocator uses dynamic, performance-oriented heuristics
 * (like bank-conflict avoidance) rather than just fitting symbols into static sections.
 *
 * =====================================================================================
 */

struct MemoryLocation {
    uint32_t bank_id;
    uint64_t address;
    uint64_t size;
};

class MemoryAllocator {
public:
    MemoryAllocator(const F1TargetMachine& target_machine);

    // Allocates all unique data labels from a set of instructions to physical SRAM locations.
    std::map<std::string, MemoryLocation> allocate(const std::vector<Inst>& instructions, F1ModelContext& context);

private:
    const F1TargetMachine& _target_machine;
    std::vector<BufferAllocator> _bank_allocators;
    uint32_t _next_bank_id;
};

} // namespace f1