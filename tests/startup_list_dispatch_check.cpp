// Checks src/startup_stubs/list_dispatch.h without a device: when command-list slot 14 (Dispatch) is forwarded to the
// desktop list and which refusal applies otherwise. Expectations are written out below, not computed with the header.
//
//   startup_list_dispatch_check [--mutate no-limit|no-state]
//
// The mutations change the expectations to those of a wrong decision (no group-count limit, or a forward without a
// compute pipeline and root); each must make the check fail.
#include "startup_stubs/list_dispatch.h"

#include <cstdio>
#include <cstring>

namespace {
using namespace x2w_startup;
using D = DispatchDecision;
int failures, cases;
bool no_limit, no_state;

void expect(const char* label, const ListDispatch& in, D expected)
{
    ++cases;
    const D got = decide_dispatch(in);
    if (got == expected) return;
    ++failures;
    std::printf("FAIL %s: expected '%s', got '%s'\n", label, dispatch_reason(expected), dispatch_reason(got));
}

ListDispatch ready(UINT x, UINT y, UINT z)
{
    ListDispatch d;
    d.list_open = true;
    d.compute_pipeline = d.compute_root = true;
    d.x = x, d.y = y, d.z = z;
    return d;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--mutate")) {
        no_limit = !std::strcmp(argv[2], "no-limit");
        no_state = !std::strcmp(argv[2], "no-state");
        if (!no_limit && !no_state) return 2;
    }
    expect("one group", ready(1, 1, 1), D::forward);
    expect("limit on every axis", ready(65535, 65535, 65535), D::forward);
    expect("zero groups", ready(0, 5, 5), D::forward);
    expect("x over limit", ready(65536, 1, 1), no_limit ? D::forward : D::group_count_over_limit);
    expect("y over limit", ready(1, 65536, 1), no_limit ? D::forward : D::group_count_over_limit);
    expect("z over limit", ready(1, 1, 0xffffffffu), no_limit ? D::forward : D::group_count_over_limit);
    ListDispatch d = ready(8, 1, 1);
    d.compute_pipeline = false;
    expect("graphics pipeline bound", d, no_state ? D::forward : D::no_compute_state);
    d = ready(8, 1, 1);
    d.compute_root = false;
    expect("no compute root", d, no_state ? D::forward : D::no_compute_state);
    d = ready(8, 1, 1);
    d.list_open = false;
    expect("closed list", d, D::list_closed);
    d = ready(8, 1, 1);
    d.type = D3D12_COMMAND_LIST_TYPE_COPY;
    expect("copy list", d, D::wrong_list_type);
    d.type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    expect("compute list", d, D::forward);
    if (failures) {
        std::printf("%d of %d cases failed\n", failures, cases);
        return 1;
    }
    std::printf("%d cases passed\n", cases);
    return 0;
}
