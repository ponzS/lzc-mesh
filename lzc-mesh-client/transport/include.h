#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "input_gate.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Handle MeshDirect;
typedef void (*MeshDirectCallback)(void *context, const char *json);
MeshDirect *mesh_direct_new(const char *id, MeshDirectCallback callback, void *context);
void mesh_direct_hub(MeshDirect *handle, const char *json);
bool mesh_direct_input(MeshDirect *handle, const char *json);
void mesh_direct_disconnect(MeshDirect *handle);
void mesh_direct_free(MeshDirect *handle);

uint64_t mesh_direct_now_us(void);
void mesh_direct_applied(MeshDirect *handle, const char *json);

// The native adapter must securely persist an exported new key before using it.
// Nonempty invalid stored keys fail closed; returned strings use the free below.
typedef struct DeviceIdentity MeshIdentity;
MeshIdentity *mesh_identity_new(const char *stored);
void mesh_identity_free(MeshIdentity *identity);
char *mesh_identity_export(const MeshIdentity *identity);
char *mesh_identity_public(const MeshIdentity *identity);
char *mesh_identity_fingerprint(const MeshIdentity *identity);
char *mesh_identity_proof(const MeshIdentity *identity, const char *user, const char *id, const char *nonce);
void mesh_identity_string_free(char *text);

#ifdef __cplusplus
}
#endif
