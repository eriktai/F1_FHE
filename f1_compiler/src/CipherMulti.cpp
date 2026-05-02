#pragma once

#include "NTT.hpp"
#include "PolynomialRing.hpp"
namespace f1 {



class CTMultiplication {
    DAG* mp;
public:


    CTMultiplication(DAG* dag) : mp(dag) {}

    
    std::pair<CipherTextNodePtr, RnsPolyNodePtr>
    run(CipherTextNodePtr ct1, CipherTextNodePtr ct2) {
        auto deg = ct1->getDegree();        
        auto L = ct1->getModulusSize();

        
        auto c0 = ModMulOpNode::op(ct1->at(0), ct2->at(0), mp);

        auto c11 = ModMulOpNode::op(ct1->at(0), ct2->at(1), mp);
        auto c12 = ModMulOpNode::op(ct1->at(1), ct2->at(0), mp);
        auto c1 = ModAddOpNode::op(c11, c12, mp);
        
        auto c2 = ModMulOpNode::op(ct1->at(1), ct2->at(1), mp);
        
        auto ct = mp->allocNodeAs<CipherTextNode>(c0, c1);
        
        //* key switching
        return {ct, c2};
    }



};







}