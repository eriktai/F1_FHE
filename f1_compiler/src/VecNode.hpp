#pragma once

#include "DAG.hpp"
#include "PolynomialRing.hpp"
#include <memory>

namespace f1 {


class VecNode : public DataNode {
public:

    VecNode(uint64_t id, const std::vector<NodePtr>& data)
    : DataNode(id, DataType::Vec)
    , buf(data)
    {}

    std::vector<NodePtr>::iterator begin() {
        return buf.begin();
    }

    std::vector<NodePtr>::iterator end() {
        return buf.end();
    }

    uint32_t getRowDim() const {
        return buf.size();
    }

    uint32_t getColDim() const {

        if (auto node = std::dynamic_pointer_cast<CoeffVecNode>(buf[0])) {
            return node->getDegree();
        } else if (auto node = std::dynamic_pointer_cast<VecNode>(buf[0])) {
            return node->getRowDim() * node->getColDim();
        }

        throw std::runtime_error("Not a correct coefficient vector type");
    }

    NodePtr& at(int idx) {
        return buf[idx];
    }


private:
    std::vector<NodePtr> buf; 
};

using VecNodePtr = std::shared_ptr<VecNode>;


class VecNodeHelper {
public:

    static VecNodePtr createCoeffTile(int M, int N, DAG* mp) {
        std::vector<NodePtr> d;
        for (int i = 0; i < M; i++) {
            d.push_back(mp->allocNodeAs<CoeffVecNode>(N));
        }
        auto rs_node = mp->allocNodeAs<VecNode>(d);
        return rs_node;
    }
};

}