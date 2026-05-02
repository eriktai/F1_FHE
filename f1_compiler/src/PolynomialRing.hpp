#ifndef __SRC_POLYNOMIALRING_HH__
#define __SRC_POLYNOMIALRING_HH__


#include <cassert>
#include <memory>
#include <vector>
#include <cstdint>
#include "DAG.hpp"

namespace f1 {


class CoeffVecNode : public DataNode {
public:

    CoeffVecNode(uint64_t id, uint32_t degree)
    : DataNode(id)
    , _degree(degree)
    {
        setName("COEFF");
        setDataType(DataType::CoeffVec);
    }

    uint32_t getDegree() const { return _degree; }

    virtual uint64_t dataSize() const override {
        return _degree * sizeof(uint32_t);
    }

private:
    uint32_t _degree;
};

using CoeffVecNodePtr = std::shared_ptr<CoeffVecNode>;
using CoeffVecNodeVec = std::vector<CoeffVecNodePtr>;

class ModulusNode : public DataNode {
public:

    ModulusNode(uint64_t id)
    : DataNode(id, DataType::KeyModulus)
    {
        setName("Q");
        setLiteral(true);
        // setDataType(DataType::KeyModulus);
    }

    virtual uint64_t dataSize() const override {
        return sizeof(uint32_t);
    }


private:
    std::string reference;
};

using ModulusNodePtr = std::shared_ptr<ModulusNode>;
using ModulusNodeVec = std::vector<ModulusNodePtr>;


class RnsPolyNode : public DataNode {
public:

    RnsPolyNode(uint64_t node_id, uint32_t degree, uint32_t L, DAG* dag)
    : DataNode(node_id, DataType::RnsPoly)
    , _modulus_size(L)
    , _degree(degree)
    , _modulus_nodes()
    , _coeff_vec_nodes()
    {
        setName("RNS");
        init(dag, degree, L);
        // printf("init size: %lu\n", _coeff_vec_nodes.size());
        // setDataType(DataType::RnsPoly);
    }

    void init(DAG* dag, uint32_t degree, uint32_t L) {
        for (int i = 0; i < L; i++) {
            _modulus_nodes.push_back(dag->allocNodeAs<ModulusNode>());
            _coeff_vec_nodes.push_back(dag->allocNodeAs<CoeffVecNode>(degree));
        }
    }

    uint32_t getModulusSize() const { return _modulus_size; }
    uint32_t getDegree() const { return _degree; }

    void setModulusSize(uint64_t size) {
        _modulus_size = size;
    }
    void setDegree(uint32_t degree) {
        _degree = degree;
    }

    CoeffVecNodePtr& operator[](uint32_t index) {
        return _coeff_vec_nodes[index];
    }

    CoeffVecNodePtr& at(uint32_t index) {
        return _coeff_vec_nodes[index];
    }

    uint64_t dataSize() const override {
        uint64_t size = 0;
        for (auto& coeff_vec : _coeff_vec_nodes) {
            size += coeff_vec->dataSize();
        }
        // for (auto& mod : _modulus_nodes) {
        //     size += mod->dataSize();
        // }
        return size;
    }

    virtual void annotate(std::string name) override {
        this->setSymbolicName(name);
        for (int i = 0; i < _modulus_size; i++) {
            _coeff_vec_nodes[i]->setSymbolicName(name + "[" + std::to_string(i) + "]");
        }
    }

private:
    uint32_t _modulus_size;
    uint32_t _degree;
    ModulusNodeVec _modulus_nodes;
    CoeffVecNodeVec _coeff_vec_nodes;
};

using RnsPolyNodePtr = std::shared_ptr<RnsPolyNode>;

class CipherTextNode : public DataNode {
public:

    CipherTextNode(uint64_t node_id, uint32_t degree, uint32_t L, DAG* dag)
    : DataNode(node_id, DataType::Ciphertext)
    {
        setName("CT");
        init(dag, degree, L);
        // setDataType(DataType::Ciphertext);
    }

    CipherTextNode(uint64_t node_id, RnsPolyNodePtr ct0, RnsPolyNodePtr ct1)
    : DataNode(node_id, DataType::Ciphertext)
    {
        setName("CT");
        rns_polys[0] = ct0;
        rns_polys[1] = ct1;
    }

    void init(DAG* dag, uint32_t degree, uint32_t L) {
        rns_polys[0] = dag->allocNodeAs<RnsPolyNode>(degree, L, dag);
        rns_polys[1] = dag->allocNodeAs<RnsPolyNode>(degree, L, dag);
    }

    RnsPolyNodePtr& operator[](uint32_t index) {
        assert(index < 2 && "The index should less than 2");
        return rns_polys[index];
    }

    RnsPolyNodePtr& at(uint32_t index) {
        assert(index < 2 && "The index should less than 2");
        return rns_polys[index];
    }

    CoeffVecNodePtr& at(uint32_t i, uint32_t j) {
        auto rns = rns_polys[i];
        auto& coeff = rns->at(j);
        return coeff;
    }

    uint64_t dataSize() const override {
        uint64_t size = 0;
        for (auto& rns_poly : rns_polys) {
            size += rns_poly->dataSize();
        }
        return size;
    }

    virtual void annotate(std::string name) override {
        this->setSymbolicName(name);
        for (int i = 0; i < 2; i++) {
            rns_polys[i]->setSymbolicName(name + "[" + std::to_string(i) + "]");
            for (int j = 0; j < rns_polys[i]->getModulusSize(); j++) {
                rns_polys[i]->at(j)->setSymbolicName(name + "[" + std::to_string(i) + "][" + std::to_string(j) + "]");
            }
        }
    }

    uint32_t getDegree() const {
        return rns_polys[0]->getDegree();
    }

    uint32_t getModulusSize() const {
        return rns_polys[0]->getModulusSize();
    }

private:
    std::array<RnsPolyNodePtr, 2> rns_polys;

};

using CipherTextNodePtr = std::shared_ptr<CipherTextNode>;

class SecretKeyNode : public DataNode {
public:

private:
    RnsPolyNodePtr rns_poly;
};

using SecretKeyNodePtr = std::shared_ptr<SecretKeyNode>;

class PublicKeyNode : public DataNode {
public:

    PublicKeyNode(uint64_t id, uint32_t degree, uint32_t L, DAG* dag)
    : DataNode(id, DataType::PublicKey)
    {
        setName("PK");
        init(dag, degree, L);
        // setDataType(DataType::PublicKey);
    }

    void init(DAG* dag, uint32_t degree, uint32_t L) {
        rns_polys[0] = dag->allocNodeAs<RnsPolyNode>(degree, L, dag);
        rns_polys[1] = dag->allocNodeAs<RnsPolyNode>(degree, L, dag);
    }

    RnsPolyNodePtr& operator[](uint32_t index) {
        assert(index < 2 && "The index should less than 2");
        return rns_polys[index];
    }

    RnsPolyNodePtr& at(uint32_t index) {
        assert(index < 2 && "The index should less than 2");
        return rns_polys[index];
    }

    CoeffVecNodePtr& at(uint32_t i, uint32_t j) {
        auto rns = rns_polys[i];
        auto& coeff = rns->at(j);
        return coeff;
    }

    uint64_t dataSize() const override {
        uint64_t size = 0;
        for (auto& rns_poly : rns_polys) {
            size += rns_poly->dataSize();
        }
        return size;
    }

    virtual void annotate(std::string name) override {
        this->setSymbolicName(name);
        for (int i = 0; i < 2; i++) {
            rns_polys[i]->setSymbolicName(name + "[" + std::to_string(i) + "]");
            for (int j = 0; j < rns_polys[i]->getModulusSize(); j++) {
                rns_polys[i]->at(j)->setSymbolicName(name + "[" + std::to_string(i) + "][" + std::to_string(j) + "]");
            }
        }
    }

private:
    std::array<RnsPolyNodePtr, 2> rns_polys;
};

using PublicKeyNodePtr = std::shared_ptr<PublicKeyNode>;

class KeySwitchKeyNode : public DataNode {
public:

    KeySwitchKeyNode(uint64_t id, uint32_t degree, uint32_t L, DAG* dag)
    : DataNode(id, DataType::KeySwitchKey)
    {
        setName("KSK");
        init(dag, degree, L);
        // setDataType(DataType::KeySwitchKey);
    }

    void init(DAG* dag, uint32_t degree, uint32_t L) {
        for (int i = 0; i < L; i++) {
            auto pk = dag->allocNodeAs<PublicKeyNode>(degree, L, dag);
            public_keys.push_back(pk);
        }
        printf("KSK L = %lu\n", public_keys.size());
    }

    PublicKeyNodePtr operator[](uint32_t index) {
        return public_keys[index];
    }

    PublicKeyNodePtr& at(uint32_t i) {
        return public_keys[i]; 
    }

    RnsPolyNodePtr& at(uint32_t i, uint32_t j) {
        return public_keys[i]->at(j);
    }


    CoeffVecNodePtr& at(uint32_t i, uint32_t j, uint32_t k) {
        return this->at(i, j)->at(k);
    }

    uint32_t L() {
        return public_keys.size();
    }

    uint64_t dataSize() const override {
        uint64_t size = 0;
        for (auto& pk : public_keys) {
            size += pk->dataSize();
        }
        return size;
    }

    virtual void annotate(std::string name) override {
        this->setSymbolicName(name);
        for (int i = 0; i < public_keys.size(); i++) {
            public_keys[i]->setSymbolicName(name + "[" + std::to_string(i) + "]");
            for (int j = 0; j < 2; j++) {
                auto rns = public_keys[i]->at(j);
                rns->setSymbolicName(name + "[" + std::to_string(i) + "][" + std::to_string(j) + "]");
                for (int k = 0; k < rns->getModulusSize(); k++) {
                    rns->at(k)->setSymbolicName(name + "[" + std::to_string(i) + "][" + std::to_string(j) + "][" + std::to_string(k) + "]");
                }
            }
        }
    }

private:
    std::vector<PublicKeyNodePtr> public_keys;
};

using KeySwitchKeyNodePtr = std::shared_ptr<KeySwitchKeyNode>;


}




#endif