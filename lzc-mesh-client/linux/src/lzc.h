#pragma once
// Discovers the local LazyCat desktop client or hclient-cli, the selected box,
// network state and the session cookie used to reach the LZC Mesh LPK.
void lzc_refresh(void);
// Drops the cached cookie so the next refresh fetches a fresh one.
void lzc_invalidate_cookie(void);
char *lzc_hub_url(void);      // wss://lzc-mesh.<box>/api/ws
char *lzc_manage_url(void);   // https://lzc-mesh.<box>/
