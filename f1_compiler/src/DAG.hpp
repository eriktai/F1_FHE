#ifndef __SRC_DAG_HH__
#define __SRC_DAG_HH__

#include "format.hpp"
#include <map>
#include <vector>
#include <memory>
#include <sstream>

namespace f1 {

using node_id_t = uint64_t;

enum class NodeType {
    None,
    Register,
    Memory,
    Operator,
    Data //* The Data Node for general data type
};

enum class OperatorCategory {
    Assign,
    INTT,
    NTT,
    ModMul,
    ModAdd,
    FIGLUT, 
};

enum class DataType {
    None,
    KeySwitchKey,
    Ciphertext,
    PublicKey,
    SecretKey,
    RnsPoly,
    CoeffVec,
    KeyModulus,
    UINT32,
    UINT64,
    UINT128, // 128bits
    UINT512,
    Tile, 
    Vec,
};

class Node {
public:

    using NodePtr = std::shared_ptr<Node>;
    using PtrType = std::shared_ptr<Node>;

    Node(uint64_t id)
    : _id(id)
    , _type(NodeType::None)
    , _dtype(DataType::None)
    , _is_literal(false)
    , _cluster_hint(-1)
    {}

    Node(uint64_t id, NodeType type)
    : _id(id)
    , _type(type)
    , _reference_id(-1)
    , _is_literal(false)
    , _cluster_hint(-1)
    {}

    Node(uint64_t id, NodeType type, DataType dtype)
    : _id(id)
    , _type(type)
    , _dtype(dtype)
    , _reference_id(-1)
    , _is_literal(false)
    , _cluster_hint(-1)
    {}

    void addSrc(NodePtr node) {
        children.push_back(node);
    } 

    void setDataType(DataType dt) {
        _dtype = dt;
    }

    DataType getDataType() {
        return _dtype;
    }

    void setType(NodeType t) { _type = t; }
    NodeType getType() const { return _type; }
    
    template<typename T>
    T* getDataAs() {
        return static_cast<T*>(_data);
    }

    void setData(uint8_t* data) {
        _data = data;
    }

    Node& operator=(NodePtr& right) {
        this->addSrc(right);
        return *this;
    }

    std::string getName() const { return node_name; }
    void setName(std::string name) { node_name = name; }

    virtual std::string getIdentity() {

        std::stringstream ss;
        std::string ref_id_str = (_reference_id != -1) ? std::to_string(_reference_id) : "";
        ss << std::format("{}({}{}{})", node_name, std::to_string(_id), ref_id_str.empty() ? "" : "->", ref_id_str);
        // ss << node_name << "(" << std::to_string(_id) << "[]" << ")";
        return ss.str();
    }

    uint64_t getId() const {
        return _id;
    }

    uint64_t getReferenceId() const {
        return _reference_id;
    }

    uint64_t getEffectiveId() const {
        return (_reference_id != -1) ? _reference_id : _id;
    }

    void setReferenceId(uint64_t ref_id) {
        _reference_id = ref_id;
    }

    virtual std::string getSymblicIdentity() const {
        std::stringstream ss;
        ss << node_name << "(" << std::to_string(_id) << ")";
        return ss.str(); 
    }

    PtrType getChildren(uint32_t index) {
        return children[index];
    }

    uint32_t totalChildren() const { return children.size(); }

    virtual uint64_t dataSize() const { return 0; }

    void setIsRootNode(bool is_root) { isRootNode = is_root; }
    bool IsRootNode() const { return isRootNode; }
    void setIsInputNode(bool is_input) { isInputNode = is_input; }
    bool IsInputNode() const { return isInputNode; }

    void setSymbolicName(std::string name) { symbolic_name = name; }
    std::string getSymbolicName() const { return symbolic_name; }
    void setLiteral(bool is_literal) { _is_literal = is_literal; }
    bool isLiteral() const {
        return _is_literal;
    }

    void setClusterHint(int cluster_id, bool force = true) {
        // Only apply if forced or if it does not already have a hint
        if (!force && _cluster_hint != -1) return;
        _cluster_hint = cluster_id;
        
        // Automatically assign the source data (children) to the same cluster
        // We use force=false here to prevent overwriting hints on shared DAG nodes.
        // If a child is used by multiple parents in different clusters, or has an
        // explicit human-assigned hint, the first assigned hint is respected.
        for (auto& child : children) {
            if (child) child->setClusterHint(cluster_id, false);
        }
    }

    int getClusterHint() const { return _cluster_hint; }
    
    bool hasClusterHint() const { return _cluster_hint != -1; }

private:
    std::vector<std::shared_ptr<Node>> children;
    uint64_t _id;
    uint64_t _reference_id;
    std::string symbolic_name;
    std::string node_name;
    NodeType _type;
    DataType _dtype;
    uint8_t* _data;
    bool _is_literal;
    bool isRootNode;
    bool isInputNode;
    int _cluster_hint;
};

using NodeDescriptor = std::pair<node_id_t, std::string>;

using NodePtr = std::shared_ptr<Node>;

class DAG {
public:

    DAG() : nxt_node_id(0) {}

    template <typename T, typename ...Args>
    std::shared_ptr<T> allocNodeAs(Args&& ...args) {
        if (!std::is_base_of_v<Node, T>) {
            throw std::runtime_error("Allocated a non derivable data type");
        }
        auto node = std::make_shared<T>(nxt_node_id, args...);
        nodes[nxt_node_id] = node;
        auto dt = node->getDataType();
        data_nodes[dt].push_back(node);
        ++nxt_node_id;
        return node; 
    }


    NodePtr allocNode() {
        auto node = std::make_shared<Node>(nxt_node_id);
        nodes[nxt_node_id] = node; 
        ++nxt_node_id;
        return node;
    }

    NodePtr getNode(uint64_t node_id) {
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            return std::shared_ptr<Node>(it->second);
        }
        return nullptr; // or throw an exception
    }

    std::vector<NodePtr>& getDataNodeByDataType(DataType nt) {
        return data_nodes[nt];
    }

private:
    uint64_t nxt_node_id;
    std::map<uint64_t, std::shared_ptr<Node>> nodes;
    std::map<DataType, std::vector<NodePtr>> data_nodes;
};

class OperatorNode : public Node {
public:
    OperatorNode(uint64_t id)
    : Node(id, NodeType::Operator)
    {}
    void setOp(OperatorCategory op) {op_cat = op;}
    OperatorCategory getOp() const { return op_cat; }

private:
    OperatorCategory op_cat;
};

class DataNode : public Node {
public:
    DataNode(uint64_t id)
    : Node(id, NodeType::Data)
    {}
    DataNode(uint64_t id, DataType dt)
    : Node(id, NodeType::Data, dt)
    {}

    virtual void annotate(std::string name) {}
};



}



#endif