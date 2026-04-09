#ifndef __INST_HH__
#define __INST_HH__

#include "DAG.hpp"
#include <string>
#include <vector>
#include <map>

namespace f1 {

#define INST_LISTS(F) \
    F(NOP, "NOP") \
    F(NTT, "NTT") \
    F(INTT, "INTT") \
    F(ModAdd, "ModAdd") \
    F(ModMul, "ModMul") \
    F(Load, "Load") \
    F(Store, "Store") \

enum class InstId {
#define X(name, str) name,
    INST_LISTS(X)
#undef X
    NUM_INST
};

extern std::map<std::string, InstId> str_to_inst_id_map;


const std::map<std::string, InstId>& getInstMap();

InstId ToInstId(const std::string& str);
std::string ToInstStr(InstId id);

//* three-address code instruction
class Inst {
public:

    Inst(InstId id, NodeDescriptor dst_label, std::vector<NodeDescriptor> src_labels)
    : _id(id)
    , _dst_label(dst_label)
    , _src_labels(src_labels)
    {}

    void setInstSeqId(uint64_t seq_id) { inst_seq_id = seq_id; }
    uint64_t getInstSeqId() const { return inst_seq_id; }
    InstId getId() const { return _id; }
    const NodeDescriptor& getDstLabel() const { return _dst_label; }
    const std::vector<NodeDescriptor>& getSrcLabels() const { return _src_labels; }

private:
    uint64_t inst_seq_id;
    InstId _id;
    NodeDescriptor _dst_label;
    std::vector<NodeDescriptor> _src_labels;

    //* 
};

}

#endif