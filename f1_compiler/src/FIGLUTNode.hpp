#pragma once

#include "DAG.hpp"
#include "NTT.hpp"
#include "PolynomialRing.hpp"
#include "UINTNode.hpp"
#include "VecNode.hpp"
#include <memory>
#include <stdexcept>

#include <algorithm>

namespace f1 {


class FIGLUTOpNode : public OperatorNode {
public:
    FIGLUTOpNode(uint64_t id)
    : OperatorNode(id)
    {
        setOp(OperatorCategory::FIGLUT);
    }

    static VecNodePtr createNew(int M, int N, DAG* node_pool) {
        std::vector<NodePtr> d;
        for (int i = 0; i < M; i++) {
            d.push_back(node_pool->allocNodeAs<CoeffVecNode>(N));
        }
        auto rs_node = node_pool->allocNodeAs<VecNode>(d);
        return rs_node;
    }

public:

    static VecNodePtr op(VecNodePtr input, VecNodePtr weight, DAG* node_pool) {
        int M = weight->getRowDim(); 
        int N = input->getColDim();
        auto rs_node = createNew(M, N, node_pool);

        auto figlut_op = node_pool->allocNodeAs<FIGLUTOpNode>();
        figlut_op->addSrc(weight);
        figlut_op->addSrc(input);
        rs_node->addSrc(figlut_op);
        return rs_node;
    }
};


struct FIGLUTConfig {
    uint32_t pe_row;
    uint32_t pe_col;
    uint32_t rac_per_pe;
    uint32_t mu;
    uint32_t mpu;
};

class FIGLUTBackend {
    FIGLUTConfig cfg;
public:

    FIGLUTBackend(FIGLUTConfig config)
    : cfg(config)
    {} 

    std::vector<std::vector<VecNodePtr>>
    run(std::vector<std::vector<CoeffVecNodePtr>> input, std::vector<std::vector<UINT64NodePtr>> weight, DAG* mp) {

        int mpu_row = cfg.pe_row * cfg.rac_per_pe;

        int mpu_col = cfg.pe_col * cfg.mu;

        int M = weight.size();
        int K = weight[0].size() * 8;
        int N = input[0].size() * input[0][0]->getDegree();

        int tile_M = std::min(mpu_row, (int)weight.size());
        int tile_K = std::min((int)cfg.mpu * mpu_col, (int)input.size());
        int tile_N = input[0][0]->getDegree();

        int n_tile_M = (M + tile_M - 1) / tile_M;
        int n_tile_N = (N + tile_N - 1) / tile_N;

        std::vector<std::vector<VecNodePtr>> res(n_tile_M, std::vector<VecNodePtr>(n_tile_N));


        for (int i = 0; i < M; i += tile_M) {
            for (int j = 0; j < N; j += tile_N) {
                VecNodePtr tile_partial_sum;
                for (int k = 0; k < K; k += tile_K) {

                    std::vector<NodePtr> input_tile;
                    for (int kk = 0; kk < tile_K; kk++) {
                        std::vector<NodePtr> row;
                        for (int jj = 0; jj < tile_N / input[0][0]->getDegree(); jj++) {
                            row.push_back(input[kk][jj]);
                            // input_tile.push_back(input[kk][jj]);
                        }
                        auto row_ptr = mp->allocNodeAs<VecNode>(row);
                        input_tile.push_back(row_ptr);
                    }

                    std::vector<NodePtr> weight_tile;
                    for (int ii = 0; ii < tile_M; ii++) {
                        std::vector<NodePtr> row;
                        for (int kk = 0; kk < tile_K / 8; kk++) {
                            row.push_back(weight[ii][kk]);
                            // weight_tile.push_back(weight[ii][kk]);
                        }
                        auto row_ptr = mp->allocNodeAs<VecNode>(row);
                        weight_tile.push_back(row_ptr);
                    }

                    //* perform figlut
                    VecNodePtr input_tile_node = mp->allocNodeAs<VecNode>(input_tile); 
                    VecNodePtr weight_tile_node = mp->allocNodeAs<VecNode>(weight_tile);

                    auto ps = FIGLUTOpNode::op(input_tile_node, weight_tile_node, mp);

                    if (k > 0) {
                        tile_partial_sum = ModAddOpNode::tile_opaque_op(tile_partial_sum, ps, mp);
                    } else {
                        tile_partial_sum = ps;
                    }
                }

                res[i / tile_M][j / tile_N] = tile_partial_sum;
            }
        }


        return res;

    }
};


}