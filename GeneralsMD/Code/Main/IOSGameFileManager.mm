#import "IOSGameFileManager.h"
#import <zlib.h>

static NSString * const kGXGameFileURL = @"https://www.dropbox.com/scl/fi/11yzk5dnym9d7cm1ie45g/generals-by-spideywv.zip?rlkey=c8wtkjf7dzos0kzq31vyfmomm&st=ts5phkhp&dl=1";

@interface GXGameFileManager () <NSURLSessionDownloadDelegate>
@property(nonatomic,strong) NSURLSession *session;
@property(nonatomic,copy) GXGameFileProgressBlock progress;
@property(nonatomic,copy) GXGameFileStatusBlock status;
@property(nonatomic,copy) GXGameFileCompletionBlock completion;
@property(nonatomic,strong) NSURL *downloadURL;
@property(nonatomic,assign) NSTimeInterval startedAt;
@property(nonatomic,assign) BOOL cancelRequested;
@property(nonatomic,assign) BOOL extractionInProgress;
@end

@implementation GXGameFileManager

+ (instancetype)sharedManager {
    static GXGameFileManager *m;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ m = [GXGameFileManager new]; });
    return m;
}

- (void)downloadAndInstallGameFileWithProgress:(GXGameFileProgressBlock)progress
                                        status:(GXGameFileStatusBlock)status
                                    completion:(GXGameFileCompletionBlock)completion {
    if (self.session) {
        if (completion) completion(NO, @"Скачивание уже выполняется.");
        return;
    }

    self.progress = progress;
    self.status = status;
    self.completion = completion;
    self.startedAt = [NSDate date].timeIntervalSince1970;
    self.cancelRequested = NO;
    self.extractionInProgress = NO;

    // Никогда не оставляем старый временный ZIP от предыдущей установки.
    NSString *staleZIP = [NSTemporaryDirectory() stringByAppendingPathComponent:@"generals by spideywv.zip"];
    [[NSFileManager defaultManager] removeItemAtPath:staleZIP error:nil];

    NSURL *url = [NSURL URLWithString:kGXGameFileURL];
    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
    cfg.timeoutIntervalForRequest = 60.0;
    cfg.timeoutIntervalForResource = 60.0 * 60.0 * 6.0;
    self.session = [NSURLSession sessionWithConfiguration:cfg delegate:self delegateQueue:nil];

    if (self.status) self.status(@"Скачивание", @"Подключение к файлу игры…");
    [[self.session downloadTaskWithURL:url] resume];
}

- (void)cancelDownload {
    self.cancelRequested = YES;
    NSURLSession *s = self.session;
    self.session = nil;
    [s invalidateAndCancel];

    // Удаляем временный ZIP и при ручной отмене. Если распаковка уже идёт,
    // открытый file handle закончит текущую операцию, а путь уже исчезнет.
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"generals by spideywv.zip"];
    if (!self.extractionInProgress)
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

    if (self.completion) self.completion(NO, @"Загрузка файла игры отменена.");
    self.progress = nil;
    self.status = nil;
    self.completion = nil;
}

- (void)finish:(BOOL)success message:(NSString *)message {
    NSURLSession *s = self.session;
    self.session = nil;
    [s invalidateAndCancel];

    // ZIP никогда не остаётся после завершения, ошибки или отмены.
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"generals by spideywv.zip"];
    [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

    if (self.completion) self.completion(success, message);
    self.progress = nil;
    self.status = nil;
    self.completion = nil;
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    NSTimeInterval elapsed = MAX(0.001, [NSDate date].timeIntervalSince1970 - self.startedAt);
    double speed = (double)totalBytesWritten / elapsed;
    NSTimeInterval remaining = totalBytesExpectedToWrite > 0 && speed > 0
        ? (double)(totalBytesExpectedToWrite - totalBytesWritten) / speed : 0;
    double p = totalBytesExpectedToWrite > 0 ? (double)totalBytesWritten / (double)totalBytesExpectedToWrite : 0;
    dispatch_async(dispatch_get_main_queue(), ^{
        // Не перезаписываем detail после progress: лаунчер должен показывать
        // размер, скорость и оставшееся время до следующего callback.
        if (self.progress) self.progress(p, totalBytesWritten, totalBytesExpectedToWrite, speed, remaining);
    });
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"generals by spideywv.zip"];
    [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

    NSError *copyError = nil;
    if (![[NSFileManager defaultManager] copyItemAtURL:location toURL:[NSURL fileURLWithPath:tmp] error:&copyError]) {
        [self finish:NO message:copyError.localizedDescription ?: @"Не удалось сохранить ZIP."];
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.progress) self.progress(1.0, 1, 1, 0, 0);
        if (self.status) self.status(@"Распаковка", @"Подготовка файлов…");
    });

    // Extraction AND verification stay off the main thread. Enumerating the
    // installed game after a large ZIP previously caused an iPhone UI freeze.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            self.extractionInProgress = YES;
            NSString *message = nil;
            BOOL ok = [self extractZIPAtPath:tmp message:&message];

        // The archive is no longer needed once extraction has completed.
        // Delete it BEFORE the verification pass so the temporary ZIP never
        // competes with the installed game for disk space during the final scan.
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

        if (ok) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self.status) self.status(@"Проверка", @"Проверка файла игры…");
            });
            ok = [self verifyGameFiles:&message];
        }

            self.extractionInProgress = NO;
            dispatch_async(dispatch_get_main_queue(), ^{
                [self finish:ok message:message ?: (ok ? @"Файл игры готов." : @"Файл игры не установлен.")];
            });
        }
    });
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    if (!error) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self finish:NO message:error.localizedDescription ?: @"Ошибка скачивания."];
    });
}

static uint32_t GXRead32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t GXRead16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

- (BOOL)extractZIPAtPath:(NSString *)zipPath message:(NSString **)message {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:zipPath];
    if (!fh) {
        if (message) *message = @"Не удалось открыть ZIP.";
        return NO;
    }

    unsigned long long size = fh.seekToEndOfFile;
    if (size < 22) {
        [fh closeFile];
        if (message) *message = @"ZIP повреждён.";
        return NO;
    }

    // Read only the ZIP tail to locate EOCD; never load the archive itself into RAM.
    unsigned long long scan = MIN(size, 65557ULL);
    [fh seekToFileOffset:size - scan];
    NSData *tail = [fh readDataOfLength:(NSUInteger)scan];
    const uint8_t *b = (const uint8_t *)tail.bytes;
    NSInteger eocd = -1;
    for (NSInteger i = (NSInteger)tail.length - 22; i >= 0; --i) {
        if (GXRead32(b + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        [fh closeFile];
        if (message) *message = @"ZIP: центральный каталог не найден.";
        return NO;
    }

    uint16_t count = GXRead16(b + eocd + 10);
    uint32_t cdSize = GXRead32(b + eocd + 12);
    uint32_t cdOffset = GXRead32(b + eocd + 16);
    if (count == 0 || (unsigned long long)cdOffset + cdSize > size) {
        [fh closeFile];
        if (message) *message = @"ZIP: неподдерживаемый или повреждённый архив.";
        return NO;
    }

    // Canonical root is Documents itself. The Files app exposes Documents as the
    // user's "Generals ZH" game folder. Never create Documents/Generals ZH.
    NSString *root = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *mkdirError = nil;
    if (![fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:&mkdirError]) {
        [fh closeFile];
        if (message) *message = mkdirError.localizedDescription ?: @"Не удалось открыть папку игры.";
        return NO;
    }

    // Extract directly into the canonical Documents root. This avoids keeping a
    // second full copy of the game in NSTemporaryDirectory and avoids the large
    // disk I/O spike that previously made iPhone/iPad lag during installation.
    // We remember files written by this pass so a failed/cancelled extraction can
    // clean only its own partial files without touching launcher settings.
    NSMutableArray<NSString *> *writtenPaths = [NSMutableArray array];

    [fh seekToFileOffset:cdOffset];
    NSData *cd = [fh readDataOfLength:(NSUInteger)cdSize];
    const uint8_t *p = (const uint8_t *)cd.bytes;
    NSUInteger pos = 0;
    BOOL ok = YES;

    for (uint16_t index = 0; index < count && ok; ++index) {
        @autoreleasepool {
            if (self.cancelRequested) {
                ok = NO;
                if (message) *message = @"Загрузка файла игры отменена.";
                break;
            }

            if (pos + 46 > cd.length || GXRead32(p + pos) != 0x02014b50) {
                ok = NO;
                if (message) *message = @"ZIP: ошибка записи каталога.";
                break;
            }

            uint16_t method = GXRead16(p + pos + 10);
            uint32_t compressed = GXRead32(p + pos + 20);
            uint32_t uncompressed = GXRead32(p + pos + 24);
            uint16_t nameLen = GXRead16(p + pos + 28);
            uint16_t extraLen = GXRead16(p + pos + 30);
            uint16_t commentLen = GXRead16(p + pos + 32);
            uint32_t localOffset = GXRead32(p + pos + 42);

            if (pos + 46 + nameLen + extraLen + commentLen > cd.length) {
                ok = NO;
                if (message) *message = @"ZIP: повреждённое имя файла.";
                break;
            }

            NSData *nameData = [NSData dataWithBytes:p + pos + 46 length:nameLen];
            NSString *name = [[NSString alloc] initWithData:nameData encoding:NSUTF8StringEncoding];
            if (!name)
                name = [[NSString alloc] initWithData:nameData encoding:NSISOLatin1StringEncoding];
            if (!name) {
                ok = NO;
                if (message) *message = @"ZIP: неизвестное имя файла.";
                break;
            }

            name = [name stringByReplacingOccurrencesOfString:@"\\\\" withString:@"/"];
            while ([name hasPrefix:@"/"]) name = [name substringFromIndex:1];
            while ([name hasPrefix:@"./"]) name = [name substringFromIndex:2];

            // Strip every outer Generals ZH/ wrapper. Examples:
            // Generals ZH/INIZH.big -> INIZH.big
            // Generals ZH/Generals ZH/ZH_Generals/x -> ZH_Generals/x
            NSString *wrapper = @"Generals ZH/";
            while (name.length >= wrapper.length &&
                   [name rangeOfString:wrapper options:NSCaseInsensitiveSearch
                                   range:NSMakeRange(0, wrapper.length)].location == 0) {
                name = [name substringFromIndex:wrapper.length];
            }

            NSString *safe = [name stringByStandardizingPath];
            if (safe.length == 0) {
                pos += 46 + nameLen + extraLen + commentLen;
                continue;
            }
            if ([safe hasPrefix:@"../"] || [safe isEqualToString:@".."] || [safe containsString:@"/../"]) {
                ok = NO;
                if (message) *message = @"ZIP содержит небезопасный путь.";
                break;
            }

            pos += 46 + nameLen + extraLen + commentLen;

            // Directory entries are created implicitly by their files. All regular
            // files are extracted; ZH_Generals is never filtered out.
            if ([name hasSuffix:@"/"])
                continue;
            if (method != 0 && method != 8) {
                ok = NO;
                if (message) *message = [NSString stringWithFormat:@"ZIP: неподдерживаемый метод для %@.", name];
                break;
            }

            [fh seekToFileOffset:localOffset];
            NSData *lh = [fh readDataOfLength:30];
            if (lh.length != 30 || GXRead32((const uint8_t *)lh.bytes) != 0x04034b50) {
                ok = NO;
                if (message) *message = @"ZIP: неверная локальная запись.";
                break;
            }

            const uint8_t *lhBytes = (const uint8_t *)lh.bytes;
            uint16_t localNameLen = GXRead16(lhBytes + 26);
            uint16_t localExtraLen = GXRead16(lhBytes + 28);
            unsigned long long dataOffset =
                (unsigned long long)localOffset + 30ULL + localNameLen + localExtraLen;
            if (dataOffset + compressed > size) {
                ok = NO;
                if (message) *message = [NSString stringWithFormat:@"ZIP: файл обрезан: %@.", name];
                break;
            }

            NSString *destination = [root stringByAppendingPathComponent:safe];
            NSString *parent = [destination stringByDeletingLastPathComponent];
            NSError *dirError = nil;
            if (![fm createDirectoryAtPath:parent withIntermediateDirectories:YES attributes:nil error:&dirError]) {
                ok = NO;
                if (message) *message = dirError.localizedDescription ?: @"Не удалось создать папку файла игры.";
                break;
            }

            // The two INI files belong to the native launcher. If the archive ever
            // contains them, do not overwrite the user's current launcher settings.
            NSString *lower = safe.lowercaseString;
            BOOL protectedSettings =
                [lower isEqualToString:@"iosipadoverrides.ini"] ||
                [lower isEqualToString:@"zerohoursettings.ini"];
            if (protectedSettings && [fm fileExistsAtPath:destination])
                continue;

            [fm removeItemAtPath:destination error:nil];

            NSFileHandle *out = [NSFileHandle fileHandleForWritingAtPath:destination];
            if (!out) {
                if (![fm createFileAtPath:destination contents:nil attributes:nil]) {
                    ok = NO;
                    if (message) *message = [NSString stringWithFormat:@"Не удалось создать %@.", name];
                    break;
                }
                out = [NSFileHandle fileHandleForWritingAtPath:destination];
            }
            if (!out) {
                ok = NO;
                if (message) *message = [NSString stringWithFormat:@"Не удалось открыть %@ для записи.", name];
                break;
            }

            [writtenPaths addObject:destination];
            [fh seekToFileOffset:dataOffset];
            const NSUInteger chunkSize = 64 * 1024;
            unsigned long long remainingCompressed = compressed;

            if (method == 0) {
                while (remainingCompressed > 0 && ok) {
                    if (self.cancelRequested) {
                        ok = NO;
                        if (message) *message = @"Загрузка файла игры отменена.";
                        break;
                    }
                    NSUInteger want = (NSUInteger)MIN((unsigned long long)chunkSize, remainingCompressed);
                    NSData *chunk = [fh readDataOfLength:want];
                    if (chunk.length != want) {
                        ok = NO;
                        if (message) *message = [NSString stringWithFormat:@"ZIP: файл обрезан: %@.", name];
                        break;
                    }
                    [out writeData:chunk];
                    remainingCompressed -= want;
                }
            } else {
                z_stream zs;
                memset(&zs, 0, sizeof(zs));
                int zret = inflateInit2(&zs, -MAX_WBITS);
                if (zret != Z_OK) {
                    ok = NO;
                    if (message) *message = [NSString stringWithFormat:@"Ошибка распаковки: %@.", name];
                } else {
                    uint8_t *outBuffer = (uint8_t *)malloc(chunkSize);
                    if (!outBuffer) {
                        inflateEnd(&zs);
                        ok = NO;
                        if (message) *message = @"Недостаточно памяти для распаковки.";
                    } else {
                        unsigned long long bytesWritten = 0;
                        while (remainingCompressed > 0 && ok) {
                            if (self.cancelRequested) {
                                ok = NO;
                                if (message) *message = @"Загрузка файла игры отменена.";
                                break;
                            }
                            NSUInteger want = (NSUInteger)MIN((unsigned long long)chunkSize, remainingCompressed);
                            NSData *chunk = [fh readDataOfLength:want];
                            if (chunk.length != want) {
                                ok = NO;
                                if (message) *message = [NSString stringWithFormat:@"ZIP: файл обрезан: %@.", name];
                                break;
                            }
                            zs.next_in = (Bytef *)chunk.bytes;
                            zs.avail_in = (uInt)chunk.length;
                            remainingCompressed -= chunk.length;

                            while (zs.avail_in > 0 && ok) {
                                zs.next_out = outBuffer;
                                zs.avail_out = (uInt)chunkSize;
                                int inflateRet = inflate(&zs, Z_NO_FLUSH);
                                if (inflateRet != Z_OK && inflateRet != Z_STREAM_END) {
                                    ok = NO;
                                    if (message) *message = [NSString stringWithFormat:@"Ошибка распаковки: %@.", name];
                                    break;
                                }
                                NSUInteger produced = chunkSize - zs.avail_out;
                                if (produced > 0) {
                                    [out writeData:[NSData dataWithBytes:outBuffer length:produced]];
                                    bytesWritten += produced;
                                }
                                if (inflateRet == Z_STREAM_END) {
                                    remainingCompressed = 0;
                                    break;
                                }
                                if (zs.avail_in == 0) break;
                            }
                        }
                        if (ok && bytesWritten != uncompressed) {
                            ok = NO;
                            if (message) *message = [NSString stringWithFormat:@"ZIP: размер после распаковки не совпал для %@.", name];
                        }
                        free(outBuffer);
                        inflateEnd(&zs);
                    }
                }
            }

            [out closeFile];
            if (!ok)
                break;
        }
    }

    [fh closeFile];
    if (!ok) {
        // Roll back only files written by this installation pass. Never delete
        // iOSIPadOverrides.ini or ZeroHourSettings.ini.
        for (NSString *path in writtenPaths)
            [fm removeItemAtPath:path error:nil];
        if (message && !*message) *message = @"Распаковка файла игры отменена.";
        return NO;
    }

    if (message)
        *message = @"Файл игры распакован в Documents: INIZH.big, ZH_Generals и остальные файлы рядом с iOSIPadOverrides.ini и ZeroHourSettings.ini.";
    return YES;
}


- (BOOL)validateInstalledGameFile:(NSString **)message {
    // Единая проверка файла игры: после установки, в статусе и в Диагностике.
    // Canonical root is Documents (Generals ZH root): INIZH.big, ZH_Generals/ and all
    // game files are siblings of the two launcher-owned INI files.
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *root = documents;
    NSFileManager *fm = [NSFileManager defaultManager];

    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:root isDirectory:&isDirectory] || !isDirectory) {
        if (message) *message = @"Файл игры: НЕ УСТАНОВЛЕН — Documents недоступен.";
        return NO;
    }

    NSString *requiredArchive = [root stringByAppendingPathComponent:@"INIZH.big"];
    NSString *zhGenerals = [root stringByAppendingPathComponent:@"ZH_Generals"];
    BOOL archiveExists = [fm fileExistsAtPath:requiredArchive isDirectory:&isDirectory] && !isDirectory;
    BOOL zhGeneralsExists = [fm fileExistsAtPath:zhGenerals isDirectory:&isDirectory] && isDirectory;
    if (!archiveExists || !zhGeneralsExists) {
        if (message) *message = [NSString stringWithFormat:
            @"Файл игры: НЕ ГОТОВ — Documents/INIZH.big: %@; Documents/ZH_Generals: %@.",
            archiveExists ? @"есть" : @"нет",
            zhGeneralsExists ? @"есть" : @"нет"];
        return NO;
    }

    NSArray *items = [fm subpathsAtPath:root];
    NSUInteger files = 0;
    NSUInteger emptyFiles = 0;

    for (NSString *relative in items) {
        NSString *path = [root stringByAppendingPathComponent:relative];
        BOOL dir = NO;
        if ([fm fileExistsAtPath:path isDirectory:&dir] && !dir) {
            NSDictionary *attr = [fm attributesOfItemAtPath:path error:nil];
            unsigned long long size = attr != nil ? [attr fileSize] : 0;
            NSString *lowerName = relative.lowercaseString;
            BOOL launcherOwned =
                [lowerName isEqualToString:@"iosipadoverrides.ini"] ||
                [lowerName isEqualToString:@"zerohoursettings.ini"] ||
                [lowerName hasPrefix:@"generals-stderr"] ||
                [lowerName hasSuffix:@".zip"];
            if (launcherOwned)
                continue;
            if (size == 0)
                emptyFiles++;
            files++;
        }
    }

    // Do not require an exact file count. Zero Hour installations can contain
    // hundreds of regular files; the old 44-file rule rejected complete archives.
    // Validate the canonical critical content instead: non-empty INIZH.big and
    // a populated ZH_Generals directory.
    NSDictionary *archiveAttributes = [fm attributesOfItemAtPath:requiredArchive error:nil];
    unsigned long long archiveSize = archiveAttributes != nil ? [archiveAttributes fileSize] : 0;

    NSUInteger zhGeneralsFiles = 0;
    for (NSString *relative in [fm subpathsAtPath:zhGenerals]) {
        NSString *path = [zhGenerals stringByAppendingPathComponent:relative];
        BOOL dir = NO;
        if ([fm fileExistsAtPath:path isDirectory:&dir] && !dir) {
            NSDictionary *attr = [fm attributesOfItemAtPath:path error:nil];
            if (attr != nil && [attr fileSize] > 0)
                ++zhGeneralsFiles;
        }
    }

    if (archiveSize == 0 || zhGeneralsFiles == 0 || files == 0) {
        if (message) *message = [NSString stringWithFormat:
            @"Файл игры: НЕ ГОТОВ — INIZH.big: %@; файлов в ZH_Generals: %lu; игровых файлов: %lu.",
            archiveSize > 0 ? @"есть" : @"пустой",
            (unsigned long)zhGeneralsFiles,
            (unsigned long)files];
        return NO;
    }

    // Launcher-owned INI files are verified at their real canonical-root paths.
    NSString *documentsIOSOverrides = [root stringByAppendingPathComponent:@"iOSIPadOverrides.ini"];
    NSString *documentsZeroHourSettings = [root stringByAppendingPathComponent:@"ZeroHourSettings.ini"];
    BOOL iosOverridesExists = [fm fileExistsAtPath:documentsIOSOverrides];
    BOOL zeroHourSettingsExists = [fm fileExistsAtPath:documentsZeroHourSettings];

    if (!iosOverridesExists || !zeroHourSettingsExists) {
        if (message) *message = [NSString stringWithFormat:
            @"Файл игры: НЕ ГОТОВ — 44/44 объектов, пустых: 0; Documents/iOSIPadOverrides.ini: %@; Documents/ZeroHourSettings.ini: %@.",
            iosOverridesExists ? @"есть" : @"нет",
            zeroHourSettingsExists ? @"есть" : @"нет"];
        return NO;
    }

    if (message) *message = [NSString stringWithFormat:
        @"Файл игры: ГОТОВ — %lu игровых файлов; INIZH.big и ZH_Generals проверены; оба INI проверены.",
        (unsigned long)files];
    return YES;
}

- (BOOL)verifyGameFiles:(NSString **)message {
    return [self validateInstalledGameFile:message];
}

@end
