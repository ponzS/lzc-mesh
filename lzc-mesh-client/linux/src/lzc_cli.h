#pragma once
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include "lzc_http.h"

// hclient-cli's authenticated loopback gRPC-Web API. Credentials stay in memory.
JsonNode *lzc_cli_default_box(SoupSession *session, LzcLocalPeer *peer, const char *base,
                              const char *credential, const char *box_override,
                              char **error);
char *lzc_cli_auth_token(SoupSession *session, LzcLocalPeer *peer, const char *base,
                         const char *credential, char **error);
