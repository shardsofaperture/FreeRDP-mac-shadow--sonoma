#ifndef FREERDP_SERVER_SHADOW_MAC_CLIPBOARD_H
#define FREERDP_SERVER_SHADOW_MAC_CLIPBOARD_H

#include <freerdp/server/shadow.h>

int mac_shadow_clipboard_init(rdpShadowClient* client, BOOL legacyWin98);
void mac_shadow_clipboard_uninit(rdpShadowClient* client);

#endif
