#import "AppleOnlineTunnel.h"
#import <Foundation/Foundation.h>
#import <NetworkExtension/NetworkExtension.h>

static NSString *GXString(const char *value) {
    return value ? [NSString stringWithUTF8String:value] : @"";
}

int GeneralsXAppleOnlineStart(const char *apiBase,
                              const char *lobbyID,
                              const char *playerToken,
                              const char *role)
{
    NSString *api = GXString(apiBase);
    NSString *lobby = GXString(lobbyID);
    NSString *token = GXString(playerToken);
    NSString *playerRole = GXString(role);

    if (!api.length || !lobby.length || !token.length)
        return -1;

    __block int result = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [NETunnelProviderManager loadAllFromPreferencesWithCompletionHandler:^(NSArray<NETunnelProviderManager *> *managers, NSError *error) {
        NETunnelProviderManager *manager = managers.firstObject;
        if (!manager)
            manager = [[NETunnelProviderManager alloc] init];

        NETunnelProviderProtocol *proto = [[NETunnelProviderProtocol alloc] init];
        proto.providerBundleIdentifier = @"com.dvorov.generalszh.online.PacketTunnel";
        proto.serverAddress = @"GeneralsXZH Apple P2P";
        proto.providerConfiguration = @{
            @"apiBase": api,
            @"lobbyID": lobby,
            @"playerToken": token,
            @"role": playerRole.length ? playerRole : @"client"
        };

        manager.protocolConfiguration = proto;
        manager.localizedDescription = @"GeneralsXZH Online";
        manager.enabled = YES;

        [manager saveToPreferencesWithCompletionHandler:^(NSError *saveError) {
            if (saveError) {
                result = -2;
                dispatch_semaphore_signal(sem);
                return;
            }
            [manager loadFromPreferencesWithCompletionHandler:^(NSError *loadError) {
                if (loadError) {
                    result = -3;
                    dispatch_semaphore_signal(sem);
                    return;
                }
                NSError *startError = nil;
                NETunnelProviderSession *session = (NETunnelProviderSession *)manager.connection;
                if (![session isKindOfClass:[NETunnelProviderSession class]]) {
                    result = -4;
                } else {
                    @try {
                        [session startTunnelWithOptions:nil error:&startError];
                        result = startError ? -5 : 0;
                    } @catch (NSException *exception) {
                        result = -6;
                    }
                }
                dispatch_semaphore_signal(sem);
            }];
        }];
    }];

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 8LL * NSEC_PER_SEC));
    return result;
}

void GeneralsXAppleOnlineStop(void)
{
    [NETunnelProviderManager loadAllFromPreferencesWithCompletionHandler:^(NSArray<NETunnelProviderManager *> *managers, NSError *error) {
        (void)error;
        for (NETunnelProviderManager *manager in managers) {
            if ([manager.connection isKindOfClass:[NETunnelProviderSession class]]) {
                [(NETunnelProviderSession *)manager.connection stopTunnel];
            }
        }
    }];
}
