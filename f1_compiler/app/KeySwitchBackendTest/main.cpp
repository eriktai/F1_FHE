
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>

#include "KeySwitchBackend.hpp"

#include "DAG.hpp"
#include "NTT.hpp"
#include "PolynomialRing.hpp"



class DAGGeneratorBase {
public:

    DAGGeneratorBase(f1::DAG* dag)
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

class DataTraveler : public DAGGeneratorBase {
public:
    DataTraveler(f1::DAG* dag, std::string outpath)
    : DAGGeneratorBase(dag)
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

class InstructionGenerator : public DAGGeneratorBase {
public:

    InstructionGenerator(f1::DAG* dag, std::string outpath)
    : DAGGeneratorBase(dag)
    , fs(outpath)
    {
        
    }

    void dfsImpl(f1::NodePtr node) override {

        if (visited.count(node->getId())) {
            return;
        }

        visited.insert(node->getId());

        if (node->getType() == f1::NodeType::Operator) {
            //* print operator name
            std::stringstream ss;
            ss << node->getIdentity() << ": ";
            ss << node->getName() << " = ";

            //* print src
            for (int i = 0; i < node->totalChildren(); i++) {
                ss << node->getChildren(i)->getIdentity();
                if (i != node->totalChildren() - 1) 
                    ss << ", ";
            } 
            ss << std::endl;
            lines.push_back({node->getId(), ss.str()});
            for (int i = 0; i < node->totalChildren(); i++) {
                dfs(node->getChildren(i));
            } 
        } else if (node->getType() == f1::NodeType::Data) {
            //* print self, and src
            std::stringstream ss;
            if (node->totalChildren() == 0) {
                ss << node->getIdentity(); 
                ss << " = "; 
                ss << " SRC";
                ss << std::endl;
                lines.push_back({node->getId(), ss.str()});
            } else if (node->totalChildren() == 1 && node->getChildren(0)->getType() == f1::NodeType::Operator) {
                auto alu_name = node->getChildren(0)->getName();
                ss << std::format("{:<10} ", alu_name);
                ss << node->getIdentity() << ", "; 
                for (int i = 0; i < node->getChildren(0)->totalChildren(); i++) {
                    auto name = node->getChildren(0)->getChildren(i)->getIdentity();
                    ss << std::format("{:<10} ", name);
                    if (i != node->getChildren(0)->totalChildren() - 1) {
                        ss << ", ";
                    }
                }
                ss << std::endl;
                lines.push_back({node->getId(), ss.str()});
                for (int i = 0; i < node->getChildren(0)->totalChildren(); i++) {
                    dfs(node->getChildren(0)->getChildren(i));
                }
            } else {
                ss << node->getIdentity(); 
                ss << " = "; 
                //* print src
                for (int i = 0; i < node->totalChildren(); i++) {
                    auto child = node->getChildren(i);
                    ss << node->getChildren(i)->getIdentity();
                    if (i != node->totalChildren() - 1)
                        ss << ", ";
                }
                ss << std::endl;
                lines.push_back({node->getId(), ss.str()});
                for (int i = 0; i < node->totalChildren(); i++) {
                    dfs(node->getChildren(i));
                }
            }
        }
    }

    // void stringify(f1::PublicKeyNodePtr pk) {

    //     std::stringstream ss;
    //     auto pk_iden = pk->getIdentity();

    //     if (visited.count(pk->getId())) {
    //         return;
    //     }

    //     visited.insert(pk->getId());

    //     ss << pk_iden << std::endl;
    //     for(int i = 0; i < 2; i++) {
    //         ss << "\t" << pk->at(i)->getIdentity() << std::endl;
    //         visited.insert(pk->at(i)->getId());
    //         for(int j = 0; j < pk->at(i)->getModulusSize(); j++) {
    //             visited.insert(pk->at(i)->at(j)->getId());
    //             ss << "\t\t" << pk->at(i)->at(j)->getIdentity();
    //             if ((j+1) % 4 == 0) {
    //                 ss << "\n";
    //             } else {
    //                 ss << ", ";
    //             }
    //         }
    //     }
    //     datas.push_back({pk->getId(), ss.str()});
    // }

    // void dfs(f1::PublicKeyNodePtr pk) {

    //     auto pk_iden = pk->getIdentity();
    //     // auto p0_iden = pk->at(0)->getIdentity();
    //     // auto p1_iden = pk->at(1)->getIdentity();

    //     // pk->at(0)->setName(pk_iden + "[0](" + p0_iden + ")");
    //     // pk->at(1)->setName(pk_iden + "[1](" + p0_iden + ")");


    //     dfs(pk->at(0));
    //     dfs(pk->at(1));
    // }

    // void stringify(f1::KeySwitchKeyNodePtr ksk) {
    //     uint32_t L = ksk->L();
    //     auto ksk_iden = ksk->getIdentity();

    //     if (visited.count(ksk->getId())) {
    //         return;
    //     }

    //     visited.insert(ksk->getId());

    //     std::stringstream ss;
    //     ss << ksk_iden << std::endl;


    //     for(int i = 0; i < L; i++) {
    //         auto pk = ksk->at(i);
    //         visited.insert(pk->getId());
    //         ss << "\t" << pk->getIdentity() << std::endl;
    //         for (int j = 0; j < 2; j++) {
    //             auto rns = pk->at(j);
    //             visited.insert(rns->getId());
    //             ss << "\t\t" << rns->getIdentity() << std::endl;
    //             for(int k = 0; k < rns->getModulusSize(); k++) {
    //                 visited.insert(rns->at(k)->getId());
    //                 ss << "\t\t\t" << rns->at(k)->getIdentity();
    //                 if ((k+1) % 4 == 0) {
    //                     ss << "\n";
    //                 } else {
    //                     ss << ", ";
    //                 }
    //             } 
    //         }
    //     }
    //     datas.push_back({ksk->getId(), ss.str()});
    // }

    // void dfs(f1::KeySwitchKeyNodePtr ksk) {
    //     uint32_t L = ksk->L();
    //     auto ksk_iden = ksk->getIdentity();

    //     // std::stringstream ss;
    //     // ss << ksk_iden << std::endl;
    //     // for(int i = 0; i < L; i++) {
    //     //     auto pk = ksk->at(i);
    //     //     ss << "\t" << pk->getIdentity() << std::endl;
    //     //     for (int j = 0; j < 2; j++) {
    //     //         auto rns = pk->at(j);
    //     //         ss << "\t\t" << rns->getIdentity() << std::endl;
    //     //         for(int k = 0; k < rns->getModulusSize(); k++) {
    //     //             ss << "\t\t\t" << rns->at(k)->getIdentity() << std::endl;
    //     //         }
    //     //     }
    //     // }
    //     // datas.push_back({ksk->getId(), ss.str()});
    //     for (int i = 0; i < L; i++) {
    //         auto pk_iden = ksk->at(i)->getIdentity();
    //         // ksk->at(i)->setName(ksk_iden + "[" + std::to_string(i) + "](" + pk_iden + ")");
    //         dfs(ksk->at(i));
    //     }
    // }

    // void dfs(f1::RnsPolyNodePtr rns) {

    //     uint32_t L = rns->getModulusSize();
    //     // auto rns_iden = rns->getIdentity();
    //     // printf("rns iden: %s\n", rns_iden.c_str());
    //     // printf("L: %d\n", L);
    //     for (int i = 0 ; i < L; i++) {
    //         // auto coeff_iden = rns->at(i)->getIdentity(); 
    //         // rns->at(i)->setName(rns_iden + "[" + std::to_string(i) + "]" + "(" + coeff_iden + ")");
    //         // printf("coeff iden: %s\n", rns->at(i)->getIdentity().c_str());
    //         dfs(rns->at(i));
    //     }        
    // }

    // void stringify(f1::CipherTextNodePtr ct) {
    //     std::stringstream ss;
    //     auto pk_iden = ct->getIdentity();
    //     ss << pk_iden << std::endl;

    //     if (visited.count(ct->getId())) {
    //         return;
    //     }
    //     visited.insert(ct->getId());

    //     for(int i = 0; i < 2; i++) {
    //         ss << "\t" << ct->at(i)->getIdentity() << std::endl;
    //         visited.insert(ct->at(i)->getId());
    //         for(int j = 0; j < ct->at(i)->getModulusSize(); j++) {
    //             ss << "\t\t" << ct->at(i)->at(j)->getIdentity();
    //             visited.insert(ct->at(i)->at(j)->getId());
    //             if ((j+1) % 4 == 0) {
    //                 ss << "\n";
    //             } else {
    //                 ss << ", ";
    //             }
    //         }
    //     }
    //     datas.push_back({ct->getId(), ss.str()});
    // }

    // void dfs(f1::CipherTextNodePtr ct) {

    //     dfs(ct->at(0));
    //     dfs(ct->at(1));
    // } 

    void postTravelImpl() override {
        std::sort(lines.begin(), lines.end(), [](auto& left, auto& right) {
            return left.first < right.first;
        });
        fs << "Data flow\n";
        for (auto& line : lines) {
            fs << line.first << ", ";
            fs << line.second;
        }
        fs << "\n\n";
        fs.close();
    }

    // void print(f1::CipherTextNodePtr root_node) {
    //     printf("Start print\n");
    //     visited.clear();
    //     dfs(root_node);

    //     std::sort(lines.begin(), lines.end(), [](auto& left, auto& right) {
    //         return left.first < right.first;
    //     });

    //     visited.clear();

    //     for (int i = (uint32_t)f1::DataType::KeySwitchKey; i <= (uint32_t)f1::DataType::CoeffVec; i++) {
    //         auto& nodes = _dag->getDataNodeByDataType((f1::DataType)i);
    //         for (auto node : nodes) {
    //             switch((f1::DataType)i) {
    //             case f1::DataType::KeySwitchKey:
    //                 stringify(std::static_pointer_cast<f1::KeySwitchKeyNode>(node));
    //                 break;
    //             case f1::DataType::PublicKey:
    //                 stringify(std::static_pointer_cast<f1::PublicKeyNode>(node));
    //                 break;
    //             case f1::DataType::Ciphertext: 
    //                 stringify(std::static_pointer_cast<f1::CipherTextNode>(node));
    //                 break;
    //             default:
    //                 break;
    //             }
    //         }
    //     }

        
    //     std::sort(datas.begin(), datas.end(), [](auto& left, auto& right) {
    //         return left.first < right.first;
    //     });
    //     fs << "Data: \n";
    //     for (auto& data : datas) {
    //         fs << data.first << ", ";
    //         fs << data.second;
    //     }
    //     fs << "\n\n";
    //     fs << "Data flow\n";
    //     for (auto& line : lines) {
    //         fs << line.first << ", ";
    //         fs << line.second;
    //     }
    //     fs << "\n\n";
    //     fs.close();
    // }

private:


    std::set<uint64_t> visited;
    std::ofstream fs;

    std::vector<std::pair<uint64_t, std::string>> lines;
};

class MemoryLayoutGenerator {
public:

};


int main(void) {

    uint32_t L = 16;

    uint32_t degree = 1024 * 16;

    f1::DAG* dag = new f1::DAG();

    auto ct = dag->allocNodeAs<f1::CipherTextNode>(degree, L, dag);
    auto c2 = dag->allocNodeAs<f1::RnsPolyNode>(degree, L, dag);
    auto ksk = dag->allocNodeAs<f1::KeySwitchKeyNode>(degree, L, dag);


    f1::ModulusNodeVec modulus_vec;
    for (int i = 0; i < L; i++) {
        modulus_vec.push_back(dag->allocNodeAs<f1::ModulusNode>());
    }

    f1::KeySwitchKeyBackend ksk_backend(ct, c2, ksk, modulus_vec, dag, degree, L);
    auto root_node = ksk_backend.run();

    //* generate instruction
    DataTraveler data_traveler(dag, "ksk_data.out");
    data_traveler.travel(root_node);

    InstructionGenerator inst_gen(dag, "ksk_inst.out");
    inst_gen.travel(root_node);

    return 0; 

}