#pragma once

#import <Foundation/Foundation.h>

typedef void (^GXGameFileProgressBlock)(double progress, int64_t receivedBytes, int64_t totalBytes, double bytesPerSecond, NSTimeInterval remainingSeconds);
typedef void (^GXGameFileStatusBlock)(NSString *stage, NSString *detail);
typedef void (^GXGameFileCompletionBlock)(BOOL success, NSString *message);

@interface GXGameFileManager : NSObject

+ (instancetype)sharedManager;

- (void)downloadAndInstallGameFileWithProgress:(GXGameFileProgressBlock)progress
                                        status:(GXGameFileStatusBlock)status
                                    completion:(GXGameFileCompletionBlock)completion;

// Отменяет текущую загрузку/распаковку файла игры.
- (void)cancelDownload;

// Единственная проверка файла игры, используемая загрузкой, статусом и диагностикой.
- (BOOL)validateInstalledGameFile:(NSString **)message;

@end
