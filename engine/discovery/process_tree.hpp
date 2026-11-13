#pragma once
#include <map>
#include <vector>
#include "../core/types.hpp"
namespace plpt {
struct ProcessNode {
    ProcessRecord record;
    std::vector<ProcessId> children;
    bool parent_missing = false;
    bool pid_reused = false;
    bool invalid_parent_identity = false;
};
class ProcessTree {
public:
    static ProcessTree build(const std::vector<ProcessRecord>&,
        const std::map<ProcessId, std::uint64_t>* previous = nullptr);
    const std::map<ProcessId, ProcessNode>& nodes() const { return nodes_; }
    const ProcessNode* find(ProcessId) const;
    std::vector<ProcessRecord> ancestors(ProcessId) const;
    std::size_t size() const { return nodes_.size(); }
private:
    std::map<ProcessId, ProcessNode> nodes_;
};
} // namespace plpt
