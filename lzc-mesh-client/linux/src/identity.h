#pragma once
#include <stdbool.h>
bool identity_init(void);
const char *identity_public(void);
const char *identity_fingerprint(void);
char *identity_proof(const char *user,const char *nonce);
void identity_shutdown(void);
