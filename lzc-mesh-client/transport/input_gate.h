// Native UI callback guard. A local takeover blocks already-queued old input.
#pragma once
#include <stdbool.h>
#include <string.h>

typedef struct MeshInputGate {
    char current[129];
    char blocked[129];
} MeshInputGate;

static inline bool mesh_input_gate_accept(const MeshInputGate *gate, const char *session) {
    if (!session || !*session) return true; // authenticated legacy relay
    return strcmp(session, gate->current) == 0 && strcmp(session, gate->blocked) != 0;
}
static inline bool mesh_input_gate_begin(MeshInputGate *gate, const char *session) {
    if (!session || !*session) return true;
    size_t n = strlen(session);
    if (n >= sizeof(gate->current) || strcmp(session, gate->blocked) == 0) return false;
    memcpy(gate->current, session, n + 1);
    return true;
}
static inline void mesh_input_gate_cancel(MeshInputGate *gate) {
    memcpy(gate->blocked, gate->current, sizeof(gate->blocked));
}
