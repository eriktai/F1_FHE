#include "Inst.hpp"
#include <string>

namespace f1 {


std::map<std::string, InstId> str_to_inst_id_map = {
#define X(name, str) {str, InstId::name},
    INST_LISTS(X)
#undef X
};

const std::map<std::string, InstId>& getInstMap() {
    return str_to_inst_id_map;
}

InstId ToInstId(const std::string& str) {
    auto it = str_to_inst_id_map.find(str);
    if (it != str_to_inst_id_map.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown instruction: " + str);
}

std::string ToInstStr(InstId id) {
    switch (id) {
#define X(name, str) case InstId::name: return str;
        INST_LISTS(X)
#undef X
    default:
        throw std::runtime_error("Unknown instruction id: " + std::to_string(static_cast<int>(id)));
    }
}


}