#pragma once

#include "DAG.hpp"
#include "F1Model.hpp"
#include "FIGLUTNode.hpp"
#include "PolynomialRing.hpp"
#include <string>
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace f1 {

class FIGLUTAccelerator {
    FIGLUTConfig _config;
    uint32_t _deg;
public:
    FIGLUTAccelerator(FIGLUTConfig config, uint32_t degree) : _config(config), _deg(degree) {}

    uint64_t getLatency() {
        // TODO: derive the latency for the FIGLUTOp based on the FIGLUTConfig
        return _config.pe_col + _deg + _config.pe_col + std::log2(_config.mpu);
    }
};

class GLUTERModel {
    FIGLUTAccelerator figlut_accel;
    F1Model f1_model;
public:
    GLUTERModel(FIGLUTConfig figlut_cfg, F1TargetMachine f1_target)
    : figlut_accel(figlut_cfg, f1_target.software_cfg.degree), f1_model(std::move(f1_target))
    {}

    std::vector<F1ModelResult> compile(DAG* dag, std::vector<std::vector<CipherTextNodePtr>> root_tiles, std::string working_dir = "", bool enable_logging = false) {
        // 1. Generate the FIGLUT Instruction first with the FIGLUT Op and identify operations
        uint64_t figlut_total_latency = 0;
        uint64_t figlut_op_count = 0;

        auto& vec_nodes = dag->getDataNodeByDataType(DataType::Vec);
        for (auto& node : vec_nodes) {
            if (node->totalChildren() > 0 && node->getChildren(0)->getType() == NodeType::Operator) {
                if (std::static_pointer_cast<OperatorNode>(node->getChildren(0))->getOp() == OperatorCategory::FIGLUT) {
                    figlut_total_latency += figlut_accel.getLatency();
                    figlut_op_count++;
                }
            }
        }

        // 2. The multiplication of scaling vector, and key switch key will be bypassed to F1 Accelerator.
        // They needs to be compiled using F1Model to map those data into concrete clusters.
        std::vector<F1ModelResult> f1_results;
        for (size_t i = 0; i < root_tiles.size(); ++i) {
            for (size_t j = 0; j < root_tiles[i].size(); ++j) {
                std::string tile_dir = working_dir.empty() ? "" : working_dir + "/tile_" + std::to_string(i) + "_" + std::to_string(j);
                if (enable_logging && !tile_dir.empty()) {
                    std::filesystem::create_directories(tile_dir);
                }
                // Pass each CipherTextNodePtr into the f1_model.compile to generate instructions
                f1_results.push_back(f1_model.compile(dag, root_tiles[i][j], tile_dir, enable_logging));
            }
        }

        // 3. calculate the total latency of CipherTextModel
        uint64_t f1_total_latency = 0;
        for (const auto& res : f1_results) {
            uint64_t max_cluster_cycle = 0;
            for (uint64_t cycle : res.cluster_total_cycles) {
                max_cluster_cycle = std::max(max_cluster_cycle, cycle);
            }
            f1_total_latency += max_cluster_cycle;
        }
        uint64_t total_latency = figlut_total_latency + f1_total_latency;

        std::cout << "=== GLUTER Model Compilation Summary ===" << std::endl;
        std::cout << "FIGLUT Operations: " << figlut_op_count << std::endl;
        std::cout << "FIGLUT Total Latency: " << figlut_total_latency << " cycles" << std::endl;
        std::cout << "F1 Total Latency: " << f1_total_latency << " cycles" << std::endl;
        std::cout << "Overall Total Latency: " << total_latency << " cycles" << std::endl;
        std::cout << "========================================" << std::endl;

        return f1_results;
    }
};

}