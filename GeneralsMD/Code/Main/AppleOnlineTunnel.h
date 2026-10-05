#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Configure the Apple Online packet tunnel. The call only stores the
// lobby/session parameters and starts the system-managed tunnel.
int GeneralsXAppleOnlineStart(const char *apiBase,
                              const char *lobbyID,
                              const char *playerToken,
                              const char *role);

void GeneralsXAppleOnlineStop(void);

#ifdef __cplusplus
}
#endif
