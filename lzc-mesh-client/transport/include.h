#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct Handle MeshDirect;
typedef void (*MeshDirectCallback)(void *context, const char *json);
MeshDirect *mesh_direct_new(const char *id, MeshDirectCallback callback, void *context);
void mesh_direct_hub(MeshDirect *handle, const char *json);
bool mesh_direct_input(MeshDirect *handle, const char *json);
void mesh_direct_disconnect(MeshDirect *handle);
void mesh_direct_free(MeshDirect *handle);

uint64_t mesh_direct_now_us(void);
void mesh_direct_applied(MeshDirect *handle, const char *json);
