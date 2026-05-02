
#include "KeySwitchBackend.hpp"
#include "F1Model.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;


int main(void) {

    uint32_t L = 16;
    uint32_t degree = 1024 * 16;
    uint32_t num_clusters = 16;

    f1::DAG* dag = new f1::DAG();

    auto ct = dag->allocNodeAs<f1::CipherTextNode>(degree, L, dag);
    auto c2 = dag->allocNodeAs<f1::RnsPolyNode>(degree, L, dag);
    auto ksk = dag->allocNodeAs<f1::KeySwitchKeyNode>(degree, L, dag);

    f1::ModulusNodeVec modulus_vec;
    for (int i = 0; i < L; i++) {
        modulus_vec.push_back(dag->allocNodeAs<f1::ModulusNode>());
    }

    f1::KeySwitchKeyBackend ksk_backend(ct, c2, ksk, modulus_vec, dag, degree, L);
    auto root_node = ksk_backend.run();

    f1::NTTConfig ntt_config(128, 32, true);
    f1::ALUConfig mm_ma_config{.latency = 1, 
                            .num_src = 2, 
                            .num_dst = 1, 
                            .word_size = 32,
                            .lane = 128, 
                            .fully_pipeline = true};

    f1::MicroArchConfig micro_arch_config{
        .scratchpad_size = 1024 * 1024 * 64,
        .scratchpad_bank = 16,
        .scratchpad_bandwidth = 512,
        .vector_register_size = 512, //* 512 bytes
        .vector_register_num = 1024,
        .num_clusters = num_clusters,
        .cluster_config = {.num_NTT = 1, .num_MM = 2, .num_MA = 2, .num_mshr = 2,
                           .ntt_config = ntt_config, .mm_config = mm_ma_config, .ma_config = mm_ma_config}
    };

    f1::SoftwareConfig software_config{
        .degree = degree,
        .modulus_size = L
    };

    f1::F1TargetMachine target_machine(micro_arch_config, software_config);
    f1::F1Model model(target_machine);
    // make folder
    std::string outpath = "./f1_model_out";
    if (!fs::exists(outpath)) {
        fs::create_directory(outpath);
    }

    model.compile(dag, root_node, outpath);

    return 0;

}