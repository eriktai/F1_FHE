#pragma once

#include "FIGLUTNode.hpp"
#include "KeySwitchBackend.hpp"
#include "CipherMulti.cpp"
#include <vector>

namespace f1 {


class FIGLUT_BCQBackend {
    FIGLUTBackend figlut_be;
public:
    
    FIGLUT_BCQBackend(FIGLUTConfig config)
    : figlut_be(config)
    {}

    // Perform BCQ Matrix Multiplication: \sum_{C} (W[c] * I) * \alpha[c]
    // scale parameter is a 2D vector where scale[c][row] represents the row-wise scalar 
    // for weight plane c and the specified global row index.
    std::vector<std::vector<VecNodePtr>>
    run(std::vector<std::vector<CoeffVecNodePtr>> input, 
        std::vector<std::vector<std::vector<UINT64NodePtr>>> weight, 
        std::vector<std::vector<CoeffVecNodePtr>> scale, 
        DAG* mp) {
        
        std::vector<std::vector<VecNodePtr>> acc_res;
        
        int C = weight.size();
        for (int c = 0; c < C; ++c) {
            // Compute Matrix Multiplication for the given bit-plane
            auto plane_res = figlut_be.run(input, weight[c], mp);
            
            if (c == 0) {
                acc_res.resize(plane_res.size());
                for (size_t i = 0; i < plane_res.size(); ++i) {
                    acc_res[i].resize(plane_res[i].size());
                }
            }
            
            for (size_t i = 0; i < plane_res.size(); ++i) {
                for (size_t j = 0; j < plane_res[i].size(); ++j) {
                    int tile_M = plane_res[i][j]->getRowDim();
                    std::vector<NodePtr> scaled_elements;
                    
                    for (int r = 0; r < tile_M; ++r) {
                        auto val = std::dynamic_pointer_cast<CoeffVecNode>(plane_res[i][j]->at(r));
                        auto s = scale[c][i * tile_M + r];
                        auto scaled_val = ModMulOpNode::op(val, s, mp);
                        scaled_elements.push_back(scaled_val);
                    }
                    
                    auto scaled_tile = mp->allocNodeAs<VecNode>(scaled_elements);
                    
                    if (c == 0) {
                        acc_res[i][j] = scaled_tile;
                    } else {
                        // Accumulate the scaled tile
                        acc_res[i][j] = ModAddOpNode::tile_opaque_op(acc_res[i][j], scaled_tile, mp);
                    }
                }
            }
        }
        
        return acc_res;
    }



};

class CipherTextFIGLUT_BCQBackend {
    FIGLUTBackend figlut_be;
public:
    CipherTextFIGLUT_BCQBackend(FIGLUTConfig config) : figlut_be(config) {}

    std::vector<std::vector<CipherTextNodePtr>>
    run(std::vector<std::vector<CipherTextNodePtr>> input,
        std::vector<std::vector<std::vector<UINT64NodePtr>>> weight,
        std::vector<std::vector<CipherTextNodePtr>> scale,
        KeySwitchKeyNodePtr ksk,
        ModulusNodeVec key_modulus,
        DAG* mp) {
        
        int C = weight.size();
        int K = input.size();
        int N_ct = input[0].size();
        uint32_t deg = input[0][0]->getDegree();
        uint32_t L = input[0][0]->getModulusSize();

        std::vector<std::vector<CipherTextNodePtr>> acc_res;

        for (int c = 0; c < C; ++c) {
            // FIGLUT operations for both RNS polys (0 and 1) and all L moduli
            std::vector<std::vector<std::vector<std::vector<VecNodePtr>>>> plane_res(2, 
                std::vector<std::vector<std::vector<VecNodePtr>>>(L));

            for (int idx = 0; idx < 2; ++idx) {
                for (uint32_t l = 0; l < L; ++l) {
                    std::vector<std::vector<CoeffVecNodePtr>> figlut_in(K, std::vector<CoeffVecNodePtr>(N_ct));
                    for (int k = 0; k < K; ++k) {
                        for (int n = 0; n < N_ct; ++n) {
                            figlut_in[k][n] = input[k][n]->at(idx, l);
                        }
                    }
                    plane_res[idx][l] = figlut_be.run(figlut_in, weight[c], mp);
                }
            }

            int n_tile_M = plane_res[0][0].size();
            int n_tile_N = plane_res[0][0][0].size();

            if (c == 0) {
                int M_total = 0;
                for (int i = 0; i < n_tile_M; ++i) {
                    M_total += plane_res[0][0][i][0]->getRowDim();
                }
                acc_res.resize(M_total, std::vector<CipherTextNodePtr>(n_tile_N));
            }

            int m_offset = 0;
            for (int i = 0; i < n_tile_M; ++i) {
                int tile_M = plane_res[0][0][i][0]->getRowDim();
                for (int j = 0; j < n_tile_N; ++j) {
                    for (int r = 0; r < tile_M; ++r) {
                        int m = m_offset + r;
                        
                        // Reconstruct the ciphertext plane from FIGLUT tiles
                        auto ct_plane = mp->allocNodeAs<CipherTextNode>(deg, L, mp);
                        for (int idx = 0; idx < 2; ++idx) {
                            for (uint32_t l = 0; l < L; ++l) {
                                auto coeff = std::dynamic_pointer_cast<CoeffVecNode>(plane_res[idx][l][i][j]->at(r));
                                ct_plane->at(idx, l) = coeff;
                            }
                        }

                        // 1. Multiplication of scaling vector and cipher text
                        auto ct_scale = scale[c][m];
                        CTMultiplication ct_mult(mp);
                        auto [ct_mul, c2] = ct_mult.run(ct_plane, ct_scale);

                        // 2. Perform the KeySwitch operation
                        KeySwitchKeyBackend ks_be(ct_mul, c2, ksk, key_modulus, mp, deg, L);
                        auto ct_ks = ks_be.run();

                        // Accumulate the scaled and key-switched result
                        if (c == 0) {
                            acc_res[m][j] = ct_ks;
                        } else {
                            auto acc_c0 = ModAddOpNode::op(acc_res[m][j]->at(0), ct_ks->at(0), mp);
                            auto acc_c1 = ModAddOpNode::op(acc_res[m][j]->at(1), ct_ks->at(1), mp);
                            acc_res[m][j] = mp->allocNodeAs<CipherTextNode>(acc_c0, acc_c1);
                        }
                    }
                }
                m_offset += tile_M;
            }
        }

        return acc_res;
    }
};


}