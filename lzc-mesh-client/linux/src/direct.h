#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <json-glib/json-glib.h>
void direct_init(void);
void direct_hub(const char *json);
bool direct_input(const char *json);
void direct_disconnect(void);
void direct_shutdown(void);

uint64_t direct_now_us(void);
void direct_applied(JsonObject *trace);
