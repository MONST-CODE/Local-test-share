#include "process_tree.hpp"
#include <set>
namespace plpt {
ProcessTree ProcessTree::build(const std::vector<ProcessRecord>& records,
    const std::map<ProcessId, std::uint64_t>* previous) {
    ProcessTree tree;
    for (const auto& record : records) {
        if (record.pid.value == 0) continue;
        ProcessNode node;
        node.record = record;
        if (previous) {
            const auto it = previous->find(record.pid);
            node.pid_reused = it != previous->end() && it->second != 0 &&
                record.start_time_ticks != 0 && it->second != record.start_time_ticks;
        }
        tree.nodes_.emplace(record.pid, std::move(node));
    }
    for (auto& [pid, node] : tree.nodes_) {
        const auto ppid = node.record.ppid;
        if (ppid.value == 0) continue; // racine, pas une erreur d'observation.
        auto parent = tree.nodes_.find(ppid);
        node.parent_missing = parent == tree.nodes_.end();
        if (node.parent_missing) continue;
        node.invalid_parent_identity = ppid == pid ||
            (node.record.start_time_ticks != 0 && parent->second.record.start_time_ticks > node.record.start_time_ticks);
        if (!node.invalid_parent_identity) parent->second.children.push_back(pid);
    }
    return tree;
}
const ProcessNode* ProcessTree::find(ProcessId pid) const {
    const auto it = nodes_.find(pid);
    return it == nodes_.end() ? nullptr : &it->second;
}
std::vector<ProcessRecord> ProcessTree::ancestors(ProcessId pid) const {
    std::vector<ProcessRecord> out;
    std::set<ProcessId> visited{pid};
    const auto* cur = find(pid);
    while (cur && !cur->invalid_parent_identity && cur->record.ppid.value != 0) {
        const auto ppid = cur->record.ppid;
        if (!visited.insert(ppid).second) break; // cycles, y compris A -> B -> A.
        cur = find(ppid);
        if (cur) out.push_back(cur->record);
    }
    return out;
}
} // namespace plpt
