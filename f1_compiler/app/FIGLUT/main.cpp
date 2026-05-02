#include "FIGLUTBCQBackend.hpp"
#include "GLUTERModel.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;


int main(int argc, char* argv[]) {

    if (argc < 5) {
        throw std::runtime_error("Less than four");
    }



    uint32_t num_clusters = 16;
    uint32_t degree = 1024 * std::atoi(argv[1]);
    uint32_t M = std::atoi(argv[2]);
    uint32_t K = std::atoi(argv[3]);
    uint32_t N = degree * std::atoi(argv[4]);

    std::cout << "degree: " << degree << std::endl;
    std::cout << "M: " << M << std::endl;
    std::cout << "N: " << N << std::endl;
    std::cout << "K: " << K << std::endl;

    uint32_t L = 16;
    f1::NTTConfig ntt_config(128, 32, true);
    f1::ALUConfig mm_ma_config{.latency = 1, 
                            .num_src = 2, 
                            .num_dst = 1, 
                            .word_size = 32,
                            .lane = 128, 
                            .fully_pipeline = true};
    f1::MicroArchConfig micro_arch_config{
        .scratchpad_size = 1024 * 1024 * 64 * 1024ul,
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


    f1::FIGLUTConfig figlut_cfg {
        .pe_row = 2,
        .pe_col = 16,
        .rac_per_pe = 32,
        .mu = 4,
        .mpu = 32
    };

    f1::GLUTERModel model(figlut_cfg, target_machine);

    f1::CipherTextFIGLUT_BCQBackend backend(figlut_cfg);

    f1::DAG* dag = new f1::DAG();

    // Setup matrix dimensions
    // Matrix sizing based on the `figlut_cfg` to match 1 full tile
    int K_bits = K; // tile_K = mpu * (pe_col * mu) = 32 * (16 * 4) = 2048
    int K_uint64 = (K_bits + 8 - 1) / 8;
    int N_ct = N < degree ? 1 : ((N + degree - 1)/ degree);      // One ciphertext column for simplicity
    int C = 2;         // Number of weight bit-planes

    // 1. Generate Input Matrix
    std::vector<std::vector<f1::CipherTextNodePtr>> input(K_bits, std::vector<f1::CipherTextNodePtr>(N_ct));
    for (int k = 0; k < K_bits; ++k) {
        for (int n = 0; n < N_ct; ++n) {
            input[k][n] = dag->allocNodeAs<f1::CipherTextNode>(degree, L, dag);
        }
    }

    // 2. Generate Weight Bit-Planes
    std::vector<std::vector<std::vector<f1::UINT64NodePtr>>> weight(C, 
        std::vector<std::vector<f1::UINT64NodePtr>>(M, std::vector<f1::UINT64NodePtr>(K_uint64)));
    for (int c = 0; c < C; ++c) {
        for (int m = 0; m < M; ++m) {
            for (int k = 0; k < K_uint64; ++k) {
                weight[c][m][k] = dag->allocNodeAs<f1::UINT64Node>();
            }
        }
    }

    // 3. Generate Scaling Vectors
    std::vector<std::vector<f1::CipherTextNodePtr>> scale(C, std::vector<f1::CipherTextNodePtr>(M));
    for (int c = 0; c < C; ++c) {
        for (int m = 0; m < M; ++m) {
            scale[c][m] = dag->allocNodeAs<f1::CipherTextNode>(degree, L, dag);
        }
    }

    // 4. Generate KeySwitchKey and Modulus
    auto ksk = dag->allocNodeAs<f1::KeySwitchKeyNode>(degree, L, dag);
    f1::ModulusNodeVec key_modulus;
    for (int i = 0; i < L; ++i) {
        key_modulus.push_back(dag->allocNodeAs<f1::ModulusNode>());
    }

    // 5. Run the Backend to build the DAG
    auto start_dag = std::chrono::high_resolution_clock::now();
    auto root_tiles = backend.run(input, weight, scale, ksk, key_modulus, dag);
    auto end_dag = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_dag = end_dag - start_dag;
    std::cout << "DAG Generation Time: " << elapsed_dag.count() << " s\n";

    // 6. Compile and output instructions via GLUTERModel
    std::string outpath = "./figlut_bcq_model_out";
    if (!fs::exists(outpath)) {
        fs::create_directory(outpath);
    }

    auto start_compile = std::chrono::high_resolution_clock::now();
    model.compile(dag, root_tiles, outpath);
    auto end_compile = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_compile = end_compile - start_compile;
    std::cout << "Compilation Time: " << elapsed_compile.count() << " s\n";
    std::cout << "Total Routine Time: " << (elapsed_dag + elapsed_compile).count() << " s\n";

    return 0;
}