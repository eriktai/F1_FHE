#pragma once

#include "DAG.hpp"

namespace f1 {

template <typename T, DataType dt>
class UINTNode : public DataNode {
public:
    UINTNode(uint64_t id)
    : DataNode(id, dt)
    {}
};

using UINT32Node = UINTNode<uint32_t, DataType::UINT32>;
using UINT64Node = UINTNode<uint64_t, DataType::UINT64>; 

using UINT32NodePtr = std::shared_ptr<UINT32Node>;
using UINT64NodePtr = std::shared_ptr<UINT64Node>;

}