#ifndef __KEYSWITCHKEY_HH__
#define __KEYSWITCHKEY_HH__

#include "PolynomialRing.hpp"
#include "NTT.hpp"
#include "Utils.hpp"


namespace f1 {

class KeySwitchKeyBackend {

public:

    KeySwitchKeyBackend(CipherTextNodePtr ct,
                        RnsPolyNodePtr c2, 
                        KeySwitchKeyNodePtr ksk, 
                        ModulusNodeVec key_modulus, DAG* node_pool, uint32_t degree, uint32_t L)
    : _ct(ct)
    , _c2(c2)
    , _ksk(ksk)
    , _key_modulus(key_modulus)
    , _node_pool(node_pool)
    , _L(L)
    , _degree(degree)
    {
        _ct->annotate("ct");
        _c2->annotate("c2");
        _ksk->annotate("ksk");
        for (int i = 0; i < L; i++) {
            _key_modulus[i]->setSymbolicName("key_modulus[" + std::to_string(i) + "]");
        }
        if(!isPow2(L)) {
            throw std::runtime_error("The size of key modulus is not power of 2");
        }
    }

    CipherTextNodePtr run() {
        auto L = _L;
        auto deg = _degree;
        auto mp = _node_pool;
        //* allocate u0, u1
        auto u0 = _node_pool->allocNodeAs<RnsPolyNode>(deg, L, mp);
        u0->annotate("u0");
        auto u1 = _node_pool->allocNodeAs<RnsPolyNode>(deg, L, mp);
        u1->annotate("u1");

        auto c2_intt = _node_pool->allocNodeAs<RnsPolyNode>(deg, L, mp);
        c2_intt->annotate("c2_intt");

        // auto intt_op_node = _node_pool->allocNodeAs<INTTOpNode>();
        // auto ntt_op_node = _node_pool->allocNodeAs<NTTOpNode>();

        // auto modadd_op_node = _node_pool->allocNodeAs<ModAddOpNode>();
        // auto modmul_op_node = _node_pool->allocNodeAs<ModMulOpNode>();

        for (int i = 0; i < L; i++) {
            c2_intt->at(i) = INTTOpNode::op(_c2->at(i), _key_modulus[i], mp);
        }
        c2_intt->annotate("c2_intt");

        for (int i = 0; i < L; i++) {
            for (int j = 0; j < L; j++) {

                // CoeffVecNodePtr c2j;

                CoeffVecNodePtr c2j;

                if (i == j) {
                    c2j = _c2->at(j);
                } else {
                    c2j = NTTOpNode::op(c2_intt->at(i), _key_modulus[j], mp); 
                    c2j->setSymbolicName("c2ntt[" + std::to_string(i) + "][" + std::to_string(j) + "]");
                }
                auto mm0 = ModMulOpNode::op(c2j, _ksk->at(i, 0, j), mp);
                mm0->setSymbolicName("mm0[" + std::to_string(i) + "][" + std::to_string(j) + "]");
                u0->at(j) = ModAddOpNode::accumulate_op(u0->at(j), 
                                                   mm0,
                                                   mp);
                u0->at(j)->setSymbolicName("u0[" + std::to_string(j) + "]");

                auto mm1 = ModMulOpNode::op(c2j, _ksk->at(i, 1, j), mp);
                mm1->setSymbolicName("mm1[" + std::to_string(i) + "][" + std::to_string(j) + "]");
                u1->at(j) = ModAddOpNode::accumulate_op(u1->at(j), 
                                                    mm1,
                                                    mp);
                u1->at(j)->setSymbolicName("u1[" + std::to_string(j) + "]");
                
            }
        }

        auto new_ct = _node_pool->allocNodeAs<CipherTextNode>(deg, L, mp);
        new_ct->annotate("new_ct");
        
        // ct[0] + u0
        new_ct->at(0) =  ModAddOpNode::op(_ct->at(0), u0, mp);
        new_ct->at(0)->annotate("new_ct[0]");
        new_ct->at(1) = ModAddOpNode::op(_ct->at(1), u1, mp);
        new_ct->at(1)->annotate("new_ct[1]");

        return new_ct;
    }

private:
    CipherTextNodePtr _ct;
    RnsPolyNodePtr _c2;
    KeySwitchKeyNodePtr _ksk;
    ModulusNodeVec _key_modulus;
    DAG* _node_pool;

    uint64_t _L;
    uint64_t _degree;
};


}

#endif

