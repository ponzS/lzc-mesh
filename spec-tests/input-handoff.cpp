#include "../lzc-mesh-client/transport/input_gate.h"
#include "../lzc-mesh-client/linux/src/local_input.h"
#include <cassert>
#include <cstdio>
int main() {
    MeshInputGate gate{};
    assert(mesh_input_gate_begin(&gate,"old-session"));
    assert(mesh_input_gate_accept(&gate,"old-session"));
    mesh_input_gate_cancel(&gate);
    assert(!mesh_input_gate_accept(&gate,"old-session"));
    assert(!mesh_input_gate_begin(&gate,"old-session"));
    assert(mesh_input_gate_begin(&gate,"new-session"));
    assert(!mesh_input_gate_accept(&gate,"old-session"));
    assert(mesh_input_gate_accept(&gate,"new-session"));
    input_event e{};
    e.type=EV_SYN;e.code=SYN_REPORT;assert(!local_input_event_activity(&e));
    e.type=EV_REL;e.code=REL_X;e.value=0;assert(!local_input_event_activity(&e));
    e.value=1;assert(local_input_event_activity(&e));
    e.type=EV_KEY;e.code=KEY_F6;e.value=0;assert(!local_input_event_activity(&e));
    e.value=1;assert(local_input_event_activity(&e));
    e.value=2;assert(!local_input_event_activity(&e));
    e.type=EV_ABS;e.code=ABS_MT_TRACKING_ID;e.value=-1;assert(!local_input_event_activity(&e));
    e.value=0;assert(local_input_event_activity(&e));
    e.type=EV_SYN;e.code=SYN_DROPPED;assert(local_input_event_activity(&e));
    std::puts("PASS: native session callback gate and physical input activity classifier");
}
