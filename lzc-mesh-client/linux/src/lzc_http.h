#pragma once
#include <libsoup/soup.h>
#include <sys/types.h>

typedef struct LzcLocalPeer LzcLocalPeer;

// Pin a live CLI process. The TCP connection is separately checked at send time.
LzcLocalPeer *lzc_local_peer_new(pid_t pid, const char *name, GError **error);
void lzc_local_peer_free(LzcLocalPeer *peer);
char *lzc_read_private_file(const char *path, gsize limit, GError **error);

SoupSession *lzc_http_session_new(gboolean local);
// Local HTTP never uses proxies. Credentials are attached only after checking
// the connected peer's kernel UID. All authenticated requests reject redirects.
GBytes *lzc_http_send_authenticated(SoupSession *session, SoupMessage *message,
                                    LzcLocalPeer *peer, const char *header,
                                    const char *credential, GError **error);
