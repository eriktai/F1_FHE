#ifndef __NTT_HH__
#define __NTT_HH__

#include "PolynomialRing.hpp"
#include "VecNode.hpp"
#include "DAG.hpp"

namespace f1 {



class INTTOpNode : public OperatorNode {
public:

    using PtrType = std::shared_ptr<INTTOpNode>;

    INTTOpNode(uint64_t nxt_id)
    : OperatorNode(nxt_id)
    {
        setName("INTT");
        setOp(OperatorCategory::INTT);
    }

    CoeffVecNodePtr operator()(CoeffVecNodePtr src, ModulusNodePtr modulus, DAG* node_pool) {
        auto new_intt_node = node_pool->allocNodeAs<INTTOpNode>(); 
        new_intt_node->operate(src, modulus);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src->getDegree());
        res->addSrc(new_intt_node);
        return res;
    }

    static CoeffVecNodePtr op(CoeffVecNodePtr src, ModulusNodePtr modulus, DAG* node_pool) {
        auto new_intt_node = node_pool->allocNodeAs<INTTOpNode>(); 
        new_intt_node->operate(src, modulus);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src->getDegree());
        res->addSrc(new_intt_node);
        return res;
    }


private:

    void operate(NodePtr src, ModulusNodePtr modulus) {
        this->addSrc(src);
        this->addSrc(modulus);
    }
    
};

using INTTOpNodePtr = std::shared_ptr<INTTOpNode>;

class NTTOpNode : public OperatorNode {
public:
    using PtrType = std::shared_ptr<NTTOpNode>;

    NTTOpNode(uint64_t nxt_id)
    : OperatorNode(nxt_id)
    {

        setName("NTT");
        setOp(OperatorCategory::NTT);
    }


    CoeffVecNodePtr operator()(CoeffVecNodePtr src, ModulusNodePtr modulus, DAG* node_pool) {
        auto new_intt_node = node_pool->allocNodeAs<NTTOpNode>(); 
        new_intt_node->operate(src, modulus);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src->getDegree());
        res->addSrc(new_intt_node);
        return res;
    }


    static CoeffVecNodePtr op(CoeffVecNodePtr src, ModulusNodePtr modulus, DAG* node_pool) {
        auto new_intt_node = node_pool->allocNodeAs<NTTOpNode>(); 
        new_intt_node->operate(src, modulus);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src->getDegree());
        res->addSrc(new_intt_node);
        return res;
    }

private:
    void operate(NodePtr src, ModulusNodePtr modulus) {
        this->addSrc(src);
        this->addSrc(modulus);
    }
};

using NTTOpNodePtr = std::shared_ptr<NTTOpNode>;

class ModMulOpNode : public OperatorNode {
public:
    using PtrType = std::shared_ptr<ModMulOpNode>;

    ModMulOpNode(uint64_t nxt_id)
    : OperatorNode(nxt_id)
    {
        
        setName("ModMul");
        setOp(OperatorCategory::ModMul);
    }
    CoeffVecNodePtr operator()(CoeffVecNodePtr src1, CoeffVecNodePtr src2, DAG* node_pool) {
        auto ma_node = node_pool->allocNodeAs<ModMulOpNode>();
        ma_node->operate(src1, src2);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src1->getDegree());
        res->addSrc(ma_node);
        return res;
    }

    static CoeffVecNodePtr op(CoeffVecNodePtr src1, CoeffVecNodePtr src2, DAG* node_pool) {
        auto ma_node = node_pool->allocNodeAs<ModMulOpNode>();
        ma_node->operate(src1, src2);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src1->getDegree());
        res->addSrc(ma_node);
        return res;
    }

    static void op(CoeffVecNodePtr& dst, CoeffVecNodePtr src1, CoeffVecNodePtr src2, DAG* mp) {
        auto ma_node = mp->allocNodeAs<ModMulOpNode>();
        ma_node->operate(src1, src2);
        dst->addSrc(ma_node);
    }

    static RnsPolyNodePtr op(RnsPolyNodePtr src1, RnsPolyNodePtr src2, DAG* mp) {
        auto L = src1->getModulusSize();
        auto deg = src1->getDegree();
        auto res = mp->allocNodeAs<RnsPolyNode>(deg, L, mp);
        for (int i = 0; i < L; i++) {
            op(res->at(i), src1->at(i), src2->at(i), mp);
        }
        return res;
    }


private:

    void operate(NodePtr src1, NodePtr src2) {
        this->addSrc(src1);
        this->addSrc(src2);
    }
}; 

using ModMulOpNodePtr = std::shared_ptr<ModMulOpNode>;

class ModAddOpNode : public OperatorNode {
public:
    using PtrType = std::shared_ptr<ModAddOpNode>;

    ModAddOpNode(uint64_t nxt_id)
    : OperatorNode(nxt_id)
    {
        setName("ModAdd");
        setOp(OperatorCategory::ModAdd);
    }

    static CoeffVecNodePtr op(CoeffVecNodePtr src1, CoeffVecNodePtr src2, DAG* node_pool) {
        auto ma_node = node_pool->allocNodeAs<ModAddOpNode>();
        ma_node->operate(src1, src2);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src1->getDegree());
        res->addSrc(ma_node);
        return res;
    }

    static CoeffVecNodePtr accumulate_op(CoeffVecNodePtr src1_sd, CoeffVecNodePtr src2, DAG* node_pool) {
        auto ma_node = node_pool->allocNodeAs<ModAddOpNode>();
        ma_node->operate(src1_sd, src2);
        auto res = node_pool->allocNodeAs<CoeffVecNode>(src1_sd->getDegree());
        res->addSrc(ma_node); 
        res->setReferenceId(src1_sd->getEffectiveId());
        return res;

    }

    static RnsPolyNodePtr op(RnsPolyNodePtr s1, RnsPolyNodePtr s2, DAG* node_pool) {
        if (s1->getModulusSize() != s2->getModulusSize()) {
            throw std::logic_error("The modulus size of source 1 and source 2 are inconsistent");
        }

        uint32_t L = s1->getModulusSize(); 
        auto res = node_pool->allocNodeAs<RnsPolyNode>(s1->getDegree(), s1->getModulusSize(), node_pool);

        for (uint32_t i = 0; i < L; i++) {
            res->at(i) = ModAddOpNode::op(s1->at(i), s2->at(i), node_pool);
        }

        return res;
    }

    static VecNodePtr tile_opaque_op(VecNodePtr tile1_dst, VecNodePtr tile2, DAG* mp) {

        auto res_tile = VecNodeHelper::createCoeffTile(tile1_dst->getRowDim(), tile1_dst->getColDim(), mp);

        auto ma_node = mp->allocNodeAs<ModAddOpNode>();
        ma_node->operate(tile1_dst, tile2);
        res_tile->addSrc(ma_node); 
        res_tile->setReferenceId(tile1_dst->getEffectiveId()); 
        return res_tile;
    }

private:

    void operate(NodePtr s1, NodePtr s2) {
        this->addSrc(s1);
        this->addSrc(s2);
    }

};

using ModAddOpNodePtr = std::shared_ptr<ModAddOpNode>;


}



#endif