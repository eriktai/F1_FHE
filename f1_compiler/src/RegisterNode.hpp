#include "DAG.hpp"
#include <cstdint>
#include <sys/types.h>

// GCC 13 is the first version to fully support std::format.
// For older versions like GCC 11, we fall back to the {fmt} library and
// provide a compatibility alias in the std namespace.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 13
#include <fmt/format.h>
namespace std { using fmt::format; }
#else
#include <format>
#endif


namespace f1 {


class RegisterNode : public Node {
public:
    RegisterNode(uint64_t id, uint32_t cluster_id, std::pair<uint64_t, uint64_t> regs, uint32_t word_size)
    : Node(id, NodeType::Register)
    , cluster_id_(cluster_id)
    , regs_(regs)
    , word_size_(word_size)
    {
        setName("REG");
        switch(word_size) {
        case 32: //* 32 bits
            setDataType(DataType::UINT32);
            break;
        case 64:
            setDataType(DataType::UINT64);
            break;
        case 128:
            setDataType(DataType::UINT128);
            break;
        case 512:
            setDataType(DataType::UINT512);
            break;
        default:
            throw std::runtime_error("Unknown word size");
        }
    }

    virtual std::string getIdentity() override {
        return std::format("C{}::{}({}:{})", cluster_id_, getName(), regs_.first, regs_.second);
    }
    void setSymbolicId(uint64_t id) {
        symbolic_id_ = id;
    }

    uint64_t getSymbolicId() const {
        return symbolic_id_;
    }

    std::pair<uint64_t, uint64_t> getRegSpan() const {
        return regs_;
    }

private:
    uint32_t cluster_id_;
    std::pair<uint64_t, uint64_t> regs_;
    uint32_t word_size_;
    uint64_t symbolic_id_;
};

using RegisterNodePtr = std::shared_ptr<RegisterNode>;


}