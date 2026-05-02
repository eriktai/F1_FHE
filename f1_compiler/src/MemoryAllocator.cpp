#include "MemoryAllocator.hpp"
#include <set>
#include <stdexcept>

namespace f1 {

MemoryAllocator::MemoryAllocator(const F1TargetMachine& target_machine)
    : _target_machine(target_machine), _next_bank_id(0) {
    
    // Initialize an allocator for each SRAM bank based on the hardware configuration.
    uint64_t num_banks = _target_machine.micro_arch_cfg.scratchpad_bank;
    uint64_t bank_size = _target_machine.micro_arch_cfg.scratchpad_size / num_banks;

    for (int i = 0; i < num_banks; ++i) {
        _bank_allocators.emplace_back(bank_size);
    }
}

std::map<std::string, MemoryLocation> MemoryAllocator::allocate(const std::vector<Inst>& instructions, F1ModelContext& context) {
    std::map<std::string, MemoryLocation> allocation_map;
    // Use a map to associate labels with their node IDs to avoid repeated lookups and ensure we only allocate once.
    std::map<std::string, std::tuple<std::string, uint64_t>> label_to_node_id;

    // 1. Collect all unique data labels and their corresponding node IDs from the instruction stream.
    for (const auto& inst : instructions) {
        for (const auto& [node_id, identity] : inst.getSrcLabels()) {
            auto node = context.dag->getNode(node_id);
            if (!node->isLiteral()) {
                label_to_node_id.try_emplace(identity, make_tuple(node->getSymbolicName(), node_id));
            }
        }
        auto& [dst_node_id, dst_identity] = inst.getDstLabel();
        auto dst_node = context.dag->getNode(dst_node_id);
        if (!dst_node->isLiteral()) {
            label_to_node_id.try_emplace(dst_identity, make_tuple(dst_node->getSymbolicName(), dst_node_id));
            // label_to_node_id.push_back({dst_node->getSymbolicName(), dst_identity, dst_node_id});
        }
    }

    // std::sort(label_to_node_id.begin(), label_to_node_id.end(), [](const auto& a, const auto& b) {
    //     // parse the format <name>[id][id][id], i want them to be sort at name first, then its ids;
    //     auto parseName = [](const std::string& s) {
    //         size_t pos = 0;
    //         if ((pos = s.find('[', pos)) != std::string::npos) {
    //             return s.substr(0, pos);
    //         }
    //         return s;
    //     };
    //     auto aName = parseName(std::get<0>(a));
    //     auto bName = parseName(std::get<0>(b));
    //     if (aName != bName) {
    //         if (aName.length() != bName.length()) {
    //             return aName.length() < bName.length();
    //         } else {
    //             return aName < bName;
    //         }
    //     }

    //     auto parseIds = [](const std::string& s) {
    //         std::vector<int> ids;
    //         size_t pos = 0;
    //         while ((pos = s.find('[', pos)) != std::string::npos) {
    //             size_t end = s.find(']', pos);
    //             if (end != std::string::npos) {
    //                 ids.push_back(std::stoi(s.substr(pos + 1, end - pos - 1)));
    //                 pos = end + 1;
    //             } else break;
    //         }
    //         return ids;
    //     };

    //     return parseIds(std::get<1>(a)) < parseIds(std::get<1>(b));
    // });


    // 2. Allocate each unique label to a bank using a round-robin policy.
    for (const auto& [label, p] : label_to_node_id) {
        auto [sym_name, node_id] = p;
        uint64_t required_size = context.dag->getNode(node_id)->dataSize();
        if (required_size == 0) {
            continue; // Skip nodes with no data to allocate.
        }

        bool allocated = false;
        for (size_t i = 0; i < _bank_allocators.size(); ++i) {
            uint32_t current_bank_id = (_next_bank_id + i) % _bank_allocators.size();
            if (_bank_allocators[current_bank_id].canAllocate(required_size)) {
                auto span = _bank_allocators[current_bank_id].allocate(node_id, required_size);
                allocation_map[label] = {current_bank_id, span.first, required_size};
                
                _next_bank_id = (current_bank_id + 1) % _bank_allocators.size();
                allocated = true;
                break;
            }
        }

        if (!allocated) {
            throw std::runtime_error("SRAM Out of Memory: Failed to allocate " + std::to_string(required_size) + " bytes for label '" + label + "'");
        }
    }

    return allocation_map;
}

} // namespace f1