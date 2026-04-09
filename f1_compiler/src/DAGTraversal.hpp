#ifndef __DAG_TRAVERSAL_HH__
#define __DAG_TRAVERSAL_HH__

#include "DAG.hpp"
#include "PolynomialRing.hpp"
#include "NTT.hpp"

#include <set>
#include <vector>
#include <fstream>

namespace f1 {


class DAGTraversalBase {
public:

    DAGTraversalBase(f1::DAG* dag)
    : _dag(dag)
    {}

    virtual void dfsImpl(f1::NodePtr node) {};
    virtual void dfsImpl(f1::PublicKeyNodePtr pk) {};
    virtual void dfsImpl(f1::KeySwitchKeyNodePtr ksk) {};
    virtual void dfsImpl(f1::RnsPolyNodePtr rns) {};
    virtual void dfsImpl(f1::CipherTextNodePtr ct) {};  

    virtual void dfs(f1::NodePtr node) {
        dfsImpl(node);
    }

    virtual void dfs(f1::PublicKeyNodePtr pk) {
        dfsImpl(pk);
        dfs(pk->at(0));
        dfs(pk->at(1));
    }

    virtual void dfs(f1::KeySwitchKeyNodePtr ksk) {
        dfsImpl(ksk);
        uint32_t L = ksk->L();
        for (int i = 0; i < L; i++) {
            dfs(ksk->at(i));
        }
    }

    virtual void dfs(f1::RnsPolyNodePtr rns) {
        dfsImpl(rns);
        uint32_t L = rns->getModulusSize();
        for (int i = 0; i < L; i++) {
            dfs(rns->at(i));
        }
    }

    virtual void dfs(f1::CipherTextNodePtr ct) {
        dfsImpl(ct);
        dfs(ct->at(0));
        dfs(ct->at(1));
    }

    virtual void postTravelImpl() {};
 
    virtual void travel(f1::CipherTextNodePtr root_node) {
        dfs(root_node);
        postTravelImpl();
    }

    f1::DAG* DAG() const { return _dag; }

private:
    f1::DAG* _dag;
};

class DataTraveler : public DAGTraversalBase {
public:
    DataTraveler(f1::DAG* dag, std::string outpath)
    : DAGTraversalBase(dag)
    , _outpath(outpath)
    {}

    std::string memToStr(uint64_t mem_in_byte) {
        std::stringstream ss;
        if (mem_in_byte < 1024) {
            ss << mem_in_byte << " B";
        } else if (mem_in_byte < 1024 * 1024) {
            ss << mem_in_byte / 1024 << " KB";
        } else if (mem_in_byte < 1024 * 1024 * 1024) {
            ss << mem_in_byte / (1024 * 1024) << " MB";
        } else {
            ss << mem_in_byte / (1024 * 1024 * 1024) << " GB";
        }
        return ss.str();
    }

    void dfsImpl(f1::NodePtr node) override {
        if (node->getType() == f1::NodeType::Data) {
            printf("Data node: %s\n", node->getIdentity().c_str());
        }
    }

    void dfsImpl(f1::PublicKeyNodePtr pk) override {
        std::stringstream ss;
        auto pk_iden = pk->getIdentity();

        if (visited.count(pk->getId())) {
            return;
        }

        visited.insert(pk->getId());

        ss << pk_iden << " " << memToStr(pk->dataSize()) << std::endl;
        for(int i = 0; i < 2; i++) {
            ss << "\t" << pk->at(i)->getIdentity() << " "
            << memToStr(pk->at(i)->dataSize()) << std::endl;
            visited.insert(pk->at(i)->getId());
            for(int j = 0; j < pk->at(i)->getModulusSize(); j++) {
                visited.insert(pk->at(i)->at(j)->getId());
                ss << "\t\t" << pk->at(i)->at(j)->getIdentity() << " "
                << memToStr(pk->at(i)->at(j)->dataSize());
                if ((j+1) % 4 == 0) {
                    ss << "\n";
                } else {
                    ss << ", ";
                }
            }
        }
        data.push_back({pk->getId(), ss.str()});
    }

    void dfsImpl(f1::KeySwitchKeyNodePtr ksk) override {
        // printf("Key switch key node: %s\n", ksk->getIdentity().c_str());
        uint32_t L = ksk->L();
        auto ksk_iden = ksk->getIdentity();

        if (visited.count(ksk->getId())) {
            return;
        }

        visited.insert(ksk->getId());

        std::stringstream ss;
        ss << ksk_iden << " " << memToStr(ksk->dataSize()) << std::endl;
        for(int i = 0; i < L; i++) {
            auto pk = ksk->at(i);
            visited.insert(pk->getId());
            ss << "\t" << pk->getIdentity() << " "
            << memToStr(pk->dataSize()) << std::endl;
            for (int j = 0; j < 2; j++) {
                auto rns = pk->at(j);
                visited.insert(rns->getId());
                ss << "\t\t" << rns->getIdentity() << " "
                << memToStr(rns->dataSize()) << std::endl;
                for(int k = 0; k < rns->getModulusSize(); k++) {
                    visited.insert(rns->at(k)->getId());
                    ss << "\t\t\t" << rns->at(k)->getIdentity() << " "
                    << memToStr(rns->at(k)->dataSize());
                    if ((k+1) % 4 == 0) {
                        ss << "\n";
                    } else {
                        ss << ", ";
                    }
                } 
            }
        }
        data.push_back({ksk->getId(), ss.str()});
    }

    void dfsImpl(f1::RnsPolyNodePtr rns) override {
        // printf("RNS poly node: %s\n", rns->getIdentity().c_str());
    }

    void dfsImpl(f1::CipherTextNodePtr ct) override {
        std::stringstream ss;
        auto pk_iden = ct->getIdentity();

        if (visited.count(ct->getId())) {
            return;
        }
        visited.insert(ct->getId());

        ss << pk_iden << " " << memToStr(ct->dataSize()) << std::endl;
        for(int i = 0; i < 2; i++) {
            ss << "\t" << ct->at(i)->getIdentity() << " "
            << memToStr(ct->at(i)->dataSize()) << std::endl;
            visited.insert(ct->at(i)->getId());
            for(int j = 0; j < ct->at(i)->getModulusSize(); j++) {
                ss << "\t\t" << ct->at(i)->at(j)->getIdentity() << " "
                << memToStr(ct->at(i)->at(j)->dataSize());
                visited.insert(ct->at(i)->at(j)->getId());
                if ((j+1) % 4 == 0) {
                    ss << "\n";
                } else {
                    ss << ", ";
                }
            }
        }
        data.push_back({ct->getId(), ss.str()});
    }

    virtual void postTravelImpl() override {
        for (int i = (uint32_t)f1::DataType::KeySwitchKey; i <= (uint32_t)f1::DataType::CoeffVec; i++) {
            auto& nodes = this->DAG()->getDataNodeByDataType((f1::DataType)i);
            for (auto node : nodes) {
                switch((f1::DataType)i) {
                case f1::DataType::KeySwitchKey:
                    dfsImpl(std::static_pointer_cast<f1::KeySwitchKeyNode>(node));
                    break;
                case f1::DataType::PublicKey:
                    dfsImpl(std::static_pointer_cast<f1::PublicKeyNode>(node));
                    break;
                case f1::DataType::Ciphertext: 
                    dfsImpl(std::static_pointer_cast<f1::CipherTextNode>(node));
                    break;
                default:
                    break;
                }
            }
        }

        
        std::sort(data.begin(), data.end(), [](auto& left, auto& right) {
            return left.first < right.first;
        });
        std::ofstream fs(_outpath);
        fs << "Data: \n";
        for (auto& d : data) {
            fs << d.first << ", ";
            fs << d.second;
        }
        fs << "\n\n";
        fs.close();
    }

private:
    std::set<uint64_t> visited;
    std::vector<std::pair<uint64_t, std::string>> data;
    std::string _outpath;
};


}

#endif