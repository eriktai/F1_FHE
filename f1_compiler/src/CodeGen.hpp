#ifndef __CODE_GEN_HH__
#define __CODE_GEN_HH__

#include "DAG.hpp"
#include "DAGTraversal.hpp"
#include "Inst.hpp"
#include "format.hpp"

namespace f1 {


class CodeGenerator : public DAGTraversalBase {

    std::set<uint64_t> visited;
    std::vector<Inst> instructions;
    std::vector<uint64_t> inst_seq_ids;
public:
    CodeGenerator(f1::DAG* dag)
    : DAGTraversalBase(dag)
    {
        
    }

    const std::vector<Inst>& getInstructions() const {
        return instructions;
    }


    virtual void postTravelImpl() override {
        std::sort(instructions.begin(), instructions.end(), [](const Inst& a, const Inst& b) {
            return a.getInstSeqId() < b.getInstSeqId();
        });
    }
 

    virtual void dfsImpl(f1::NodePtr node) override {

        if (visited.count(node->getId())) {
            return;
        }

        visited.insert(node->getId());

        if (node->getType() == f1::NodeType::Operator) {
            //* print operator name
            // std::stringstream ss;
            // ss << node->getIdentity() << ": ";
            // ss << node->getName() << " = ";

            // //* print src
            // for (int i = 0; i < node->totalChildren(); i++) {
            //     ss << node->getChildren(i)->getIdentity();
            //     if (i != node->totalChildren() - 1) 
            //         ss << ", ";
            // } 
            // ss << std::endl;
            // lines.push_back({node->getId(), ss.str()});
            // for (int i = 0; i < node->totalChildren(); i++) {
            //     dfs(node->getChildren(i));
            // } 
            throw std::runtime_error("Operator node should not be visited directly");
        } else if (node->getType() == f1::NodeType::Data) {
            //* print self, and src
            std::stringstream ss;
            if (node->totalChildren() == 0) {

                // ss << node->getIdentity(); 
                // ss << " = "; 
                // ss << " SRC";
                // ss << std::endl;
                // lines.push_back({node->getId(), ss.str()});
            } else if (node->totalChildren() == 1 && node->getChildren(0)->getType() == f1::NodeType::Operator) {
                auto alu_name = node->getChildren(0)->getName();

                auto inst_id = ToInstId(alu_name);
                std::vector<NodeDescriptor> src_labels;
                NodeDescriptor dst_label = {node->getId(), node->getIdentity()};

                // ss << std::format("{:<10} ", alu_name);
                // ss << node->getIdentity() << ", "; 
                for (int i = 0; i < node->getChildren(0)->totalChildren(); i++) {
                    auto identity = node->getChildren(0)->getChildren(i)->getIdentity();
                    auto id = node->getChildren(0)->getChildren(i)->getId();
                    src_labels.push_back({id, identity}); 
                }
                Inst inst(inst_id, dst_label, src_labels);
                inst.setInstSeqId(node->getId());
                instructions.push_back(inst);
                for (int i = 0; i < node->getChildren(0)->totalChildren(); i++) {
                    dfs(node->getChildren(0)->getChildren(i));
                }
            } else {
                throw std::runtime_error("Unsupported data node with multiple operator children");
                // ss << node->getIdentity(); 
                // ss << " = "; 
                // //* print src
                // for (int i = 0; i < node->totalChildren(); i++) {
                //     auto child = node->getChildren(i);
                //     ss << node->getChildren(i)->getIdentity();
                //     if (i != node->totalChildren() - 1)
                //         ss << ", ";
                // }
                // ss << std::endl;
                // lines.push_back({node->getId(), ss.str()});
                // for (int i = 0; i < node->totalChildren(); i++) {
                //     dfs(node->getChildren(i));
                // }
            }
        }
    }
};

}

#endif