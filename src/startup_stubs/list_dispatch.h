#pragma once

// Command-list slot 14, Dispatch(x, y, z). A dispatch is forwarded to the desktop list only when the list is open,
// can run compute, a desktop compute pipeline and a desktop compute root signature are bound, and every group count
// is within the D3D12 limit of 65535 per dimension. Zero groups on any axis is valid and runs nothing.

#include <d3d12.h>

namespace x2w_startup {

constexpr UINT max_dispatch_groups = 65535;

struct ListDispatch {
    bool list_open = false;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool compute_pipeline = false;  // the bound pipeline is a desktop compute pipeline
    bool compute_root = false;      // the bound compute root signature is a desktop root signature
    UINT x = 0, y = 0, z = 0;
};

enum class DispatchDecision { forward, list_closed, wrong_list_type, no_compute_state, group_count_over_limit };
constexpr int dispatch_decision_count = 5;

inline const char* dispatch_reason(DispatchDecision decision)
{
    switch (decision) {
    case DispatchDecision::forward: return "forward";
    case DispatchDecision::list_closed: return "list is closed";
    case DispatchDecision::wrong_list_type: return "requires a direct or compute command list";
    case DispatchDecision::no_compute_state: return "requires a desktop compute pipeline and root";
    case DispatchDecision::group_count_over_limit: return "group count is over 65535";
    }
    return "unknown";
}

inline DispatchDecision decide_dispatch(const ListDispatch& d)
{
    if (!d.list_open) return DispatchDecision::list_closed;
    if (d.type != D3D12_COMMAND_LIST_TYPE_DIRECT && d.type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return DispatchDecision::wrong_list_type;
    if (!d.compute_pipeline || !d.compute_root) return DispatchDecision::no_compute_state;
    if (d.x > max_dispatch_groups || d.y > max_dispatch_groups || d.z > max_dispatch_groups)
        return DispatchDecision::group_count_over_limit;
    return DispatchDecision::forward;
}

}  // namespace x2w_startup
