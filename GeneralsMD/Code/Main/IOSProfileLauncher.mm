#include "IOSProfileLauncher.h"
#import "IOSGameFileManager.h"

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unistd.h>

#ifndef GX_LAUNCHER_COMMIT
#define GX_LAUNCHER_COMMIT "unknown"
#endif
#ifndef GX_ENGINE_COMMIT
#define GX_ENGINE_COMMIT "unknown"
#endif
#ifndef GX_BASE_SHELL_RUN
#define GX_BASE_SHELL_RUN "unknown"
#endif
#ifndef GX_PROJECT_VERSION
#define GX_PROJECT_VERSION "0.0.0"
#endif
#ifndef GX_ENGINE_VERSION
#define GX_ENGINE_VERSION "0.0.0"
#endif
#ifndef GX_LAUNCHER_VERSION
#define GX_LAUNCHER_VERSION "0.0.0"
#endif
#ifndef GX_LAUNCHER_RUN
#define GX_LAUNCHER_RUN "unknown"
#endif

namespace
{
std::atomic<bool> gLauncherFinished(false);
char gSelectedProfile[32] = "vanilla";
GeneralsXIOSDiagnosticClearCallback gDiagnosticClearCallback = nullptr;

NSString *ShortBuildIdentifier(const char *raw)
{
    if (raw == nullptr || raw[0] == '\0')
        return @"unknown";

    NSString *value = [NSString stringWithUTF8String:raw];
    if (value == nil || value.length == 0)
        return @"unknown";

    if ([value isEqualToString:@"unknown"] || value.length <= 10)
        return value;

    return [value substringToIndex:10];
}

NSString *DocumentsFilePath(NSString *name)
{
    return [[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"]
            stringByAppendingPathComponent:name];
}

NSArray<NSString *> *DiagnosticSessionLogNames()
{
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithObject:@"generals-stderr.log"];
    for (NSInteger index = 1; index <= 9; ++index)
        [names addObject:[NSString stringWithFormat:@"generals-stderr-%02ld.log", (long)index]];
    return names;
}

unsigned long long FileSizeAtPath(NSString *path)
{
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    return attributes != nil ? [attributes fileSize] : 0;
}

NSString *HumanReadableBytes(unsigned long long bytes)
{
    return [NSByteCountFormatter stringFromByteCount:(long long)bytes
                                          countStyle:NSByteCountFormatterCountStyleFile];
}

unsigned long long DirectorySizeAtPath(NSString *path)
{
    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSDirectoryEnumerator<NSString *> *enumerator = [fileManager enumeratorAtPath:path];
    if (enumerator == nil)
        return 0;

    unsigned long long total = 0;
    for (NSString *relativePath in enumerator)
    {
        NSString *fullPath = [path stringByAppendingPathComponent:relativePath];
        NSDictionary<NSFileAttributeKey, id> *attributes =
            [fileManager attributesOfItemAtPath:fullPath error:nil];
        if ([[attributes fileType] isEqualToString:NSFileTypeRegular])
            total += [attributes fileSize];
    }
    return total;
}

NSString *GameRootPath();

unsigned long long InstalledGameFilesSizeAtDocuments()
{
    NSString *documents = GameRootPath();
    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSDirectoryEnumerator<NSString *> *enumerator = [fileManager enumeratorAtPath:documents];
    if (enumerator == nil)
        return 0;

    unsigned long long total = 0;
    for (NSString *relativePath in enumerator)
    {
        NSString *lower = relativePath.lowercaseString;
        if ([lower isEqualToString:@"iosipadOverrides.ini"] ||
            [lower isEqualToString:@"zerohoursettings.ini"] ||
            [lower hasPrefix:@"generals-stderr"] ||
            [lower hasSuffix:@".zip"])
            continue;

        NSString *fullPath = [documents stringByAppendingPathComponent:relativePath];
        NSDictionary<NSFileAttributeKey, id> *attributes =
            [fileManager attributesOfItemAtPath:fullPath error:nil];
        if ([[attributes fileType] isEqualToString:NSFileTypeRegular])
            total += [attributes fileSize];
    }
    return total;
}

bool IsSupportedProfile(const char *profile)
{
    return profile != nullptr &&
        (strcmp(profile, "vanilla") == 0 ||
         strcmp(profile, "enhanced") == 0 ||
         strcmp(profile, "zerohour") == 0);
}

void SetSelectedProfile(NSString *profile)
{
    if (profile == nil)
        return;

    const char *utf8 = [profile UTF8String];
    if (!IsSupportedProfile(utf8))
        return;

    strlcpy(gSelectedProfile, utf8, sizeof(gSelectedProfile));
    fprintf(stderr, "INFO: iOS native launcher selected profile: %s\n", gSelectedProfile);
    gLauncherFinished.store(true, std::memory_order_release);
}

NSString *BundledAutoLaunchProfile()
{
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    NSString *markerPath = [resourcePath stringByAppendingPathComponent:@"AutoLaunchProfile.txt"];

    NSError *error = nil;
    NSString *value = [NSString stringWithContentsOfFile:markerPath
                                                encoding:NSUTF8StringEncoding
                                                   error:&error];
    if (value == nil)
        return nil;

    value = [value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    const char *utf8 = [value UTF8String];
    if (!IsSupportedProfile(utf8))
    {
        fprintf(stderr, "WARNING: iOS launcher ignored unsupported AutoLaunchProfile '%s'\n",
                utf8 != nullptr ? utf8 : "<null>");
        return nil;
    }

    return value;
}

NSString *GameRootPath()
{
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return documents;
}

BOOL EnsureGameRootDirectory()
{
    NSString *root = GameRootPath();
    BOOL isDirectory = NO;
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:root isDirectory:&isDirectory])
        return isDirectory;
    return [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
}

NSString *IOSIPadOverridesPath()
{
    // Documents itself is the Generals ZH root; keep the INI beside all installed game files.
    return [GameRootPath() stringByAppendingPathComponent:@"iOSIPadOverrides.ini"];
}

NSString *ZeroHourSettingsPath()
{
    // Documents itself is the Generals ZH root; keep the INI beside all installed game files.
    return [GameRootPath() stringByAppendingPathComponent:@"ZeroHourSettings.ini"];
}

NSString *GameOptionsPath()
{
    // The engine's OptionPreferences loads the canonical Options.ini from its working directory.
    return [GameRootPath() stringByAppendingPathComponent:@"Options.ini"];
}

NSString *EngineOptionsPath()
{
    NSString *dir = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/GeneralsX/GeneralsZH"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    return [dir stringByAppendingPathComponent:@"Options.ini"];
}

NSMutableDictionary<NSString *, NSString *> *ReadKeyValueFile(NSString *path)
{
    NSMutableDictionary<NSString *, NSString *> *values = [NSMutableDictionary dictionary];
    NSString *contents = [NSString stringWithContentsOfFile:path
                                                   encoding:NSUTF8StringEncoding
                                                      error:nil];
    if (contents == nil)
        return values;

    NSCharacterSet *space = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    for (NSString *rawLine in [contents componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]])
    {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:space];
        if (line.length == 0 || [line hasPrefix:@"#"] || [line hasPrefix:@";"])
            continue;

        NSRange equals = [line rangeOfString:@"="];
        if (equals.location == NSNotFound)
            continue;

        NSString *key = [[line substringToIndex:equals.location] stringByTrimmingCharactersInSet:space];
        NSString *value = [[line substringFromIndex:equals.location + 1] stringByTrimmingCharactersInSet:space];
        if (key.length > 0)
            values[key] = value;
    }
    return values;
}

NSString *SettingValue(NSDictionary<NSString *, NSString *> *values,
                       NSString *key,
                       NSString *fallback)
{
    NSString *value = values[key];
    return value.length > 0 ? value : fallback;
}

BOOL SettingBoolValue(NSDictionary<NSString *, NSString *> *values,
                      NSString *key,
                      BOOL fallback)
{
    NSString *value = [[SettingValue(values, key, fallback ? @"Yes" : @"No") lowercaseString]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    return [value isEqualToString:@"yes"] || [value isEqualToString:@"true"] ||
           [value isEqualToString:@"1"] || [value isEqualToString:@"on"];
}

BOOL WriteKeyValueFile(NSString *path, NSDictionary<NSString *, NSString *> *values, NSError **error)
{
    NSArray<NSString *> *keys =
        [[values allKeys] sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    NSMutableString *output = [NSMutableString string];
    for (NSString *key in keys)
        [output appendFormat:@"%@ = %@\n", key, values[key]];
    return [output writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:error];
}

NSDictionary<NSString *, NSString *> *DefaultZeroHourSettings()
{
    return @{
        @"ControlBar": @"ZeroHour",
        @"Cameos": @"Standard",
        @"Music": @"Standard",
        @"UnitVoices": @"English",
        @"Hotkeys": @"Original",
        @"HotkeyLanguage": @"English",
        @"Portraits": @"Standard",
        @"FogEffects": @"No",
        @"WaterEffects": @"Yes",
        @"ExtraBuildingProps": @"Yes",
        @"UseShadowVolumes": @"Yes",
        @"UseShadowDecals": @"Yes",
        @"UseCloudMap": @"No",
        @"UseLightMap": @"Yes",
        @"ShowSoftWaterEdge": @"No",
        @"BuildingOcclusion": @"Yes",
        @"ShowTrees": @"Yes",
        @"ExtraAnimations": @"Yes",
        @"DynamicLOD": @"No",
        @"HeatEffects": @"No",
        @"TextureReduction": @"0",
        @"MaxParticleCount": @"2500",
        @"TextureFilter": @"Anisotropic",
        @"AnisotropyLevel": @"8"
    };
}

void EnsureDefaultZeroHourSettings()
{
    EnsureGameRootDirectory();
    NSString *path = ZeroHourSettingsPath();
    if ([[NSFileManager defaultManager] fileExistsAtPath:path])
        return;

    NSError *error = nil;
    if (!WriteKeyValueFile(path, DefaultZeroHourSettings(), &error))
    {
        fprintf(stderr, "ERROR: failed to seed ZeroHourSettings.ini: %s\n",
                error != nil ? [[error description] UTF8String] : "unknown");
    }
}

bool ProfileDirectoryExists(NSString *profileDirectory)
{
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    NSString *path = [[resourcePath stringByAppendingPathComponent:@"Profiles"]
                      stringByAppendingPathComponent:profileDirectory];

    BOOL isDirectory = NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path
                                               isDirectory:&isDirectory] && isDirectory;
}

NSString *DefaultIOSIPadOverrides()
{
    // GeneralsX @feature dvorovrus 26/09/2026 Default shared iOS/iPad tuning.
    return @"GameData\n"
            @"  MaxCameraHeight = 550.0\n"
            @"  MinCameraHeight = 70.0\n"
            @"  CameraPitch = 37.0\n"
            @"  EnforceMaxCameraHeight = No\n"
            @"  KeyboardScrollSpeedFactor = 1.0\n"
            @"  TerrainDrawDistanceScale = 1.35\n"
            @"  UseFPSLimit = Yes\n"
            @"  FramesPerSecondLimit = 60\n"
            @"End\n";
}

void EnsureDefaultIOSIPadOverrides()
{
    EnsureGameRootDirectory();
    NSString *path = IOSIPadOverridesPath();
    if ([[NSFileManager defaultManager] fileExistsAtPath:path])
        return;

    NSError *error = nil;
    BOOL ok = [DefaultIOSIPadOverrides() writeToFile:path
                                      atomically:YES
                                        encoding:NSUTF8StringEncoding
                                           error:&error];
    if (ok)
    {
        fprintf(stderr, "INFO: iOS launcher seeded %s\n", path.fileSystemRepresentation);
    }
    else
    {
        fprintf(stderr, "ERROR: iOS launcher failed to seed iOSIPadOverrides.ini: %s\n",
                [[error description] UTF8String]);
    }
}

UIWindowScene *FindActiveWindowScene()
{
    for (UIScene *scene in [UIApplication sharedApplication].connectedScenes)
    {
        if (![scene isKindOfClass:[UIWindowScene class]])
            continue;

        if (scene.activationState == UISceneActivationStateForegroundActive ||
            scene.activationState == UISceneActivationStateForegroundInactive)
        {
            return (UIWindowScene *)scene;
        }
    }

    return nil;
}

UILabel *MakeLabel(NSString *text, CGFloat size, UIFontWeight weight)
{
    UILabel *label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.text = text;
    label.textColor = UIColor.whiteColor;
    label.textAlignment = NSTextAlignmentCenter;
    label.font = [UIFont systemFontOfSize:size weight:weight];
    label.numberOfLines = 0;
    return label;
}

UIVisualEffectView *MakeGlassBlurView(UIView *container)
{
    UIBlurEffect *effect = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
    UIVisualEffectView *blur = [[UIVisualEffectView alloc] initWithEffect:effect];
    blur.translatesAutoresizingMaskIntoConstraints = NO;
    blur.userInteractionEnabled = NO;
    blur.alpha = 0.82;
    [container addSubview:blur];
    [NSLayoutConstraint activateConstraints:@[
        [blur.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
        [blur.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
        [blur.topAnchor constraintEqualToAnchor:container.topAnchor],
        [blur.bottomAnchor constraintEqualToAnchor:container.bottomAnchor]
    ]];
    [container sendSubviewToBack:blur];
    return blur;
}

UIButton *MakeButton(NSString *title, id target, SEL action)
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:19.0 weight:UIFontWeightSemibold];
    button.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.52];
    button.layer.shadowColor = [UIColor colorWithRed:0.0 green:0.45 blue:1.0 alpha:1.0].CGColor;
    button.layer.shadowOpacity = 0.22;
    button.layer.shadowRadius = 13.0;
    button.layer.shadowOffset = CGSizeMake(0, 5);
    button.layer.cornerRadius = 10.0;
    button.layer.borderWidth = 1.0;
    button.layer.borderColor = [UIColor colorWithWhite:0.28 alpha:1.0].CGColor;
    [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintEqualToConstant:58.0].active = YES;
    return button;
}
}

@interface GXProfileLauncherViewController : UIViewController
@property(nonatomic, strong) UIStackView *menuStack;
@property(nonatomic, strong) UIView *settingsView;
@property(nonatomic, strong) UILabel *settingsStatus;
@property(nonatomic, strong) UISlider *maxCameraSlider;
@property(nonatomic, strong) UISlider *minCameraSlider;
@property(nonatomic, strong) UISlider *cameraPitchSlider;
@property(nonatomic, strong) UISlider *scrollSpeedSlider;
@property(nonatomic, strong) UISlider *drawDistanceSlider;
@property(nonatomic, strong) UISlider *fpsSlider;
@property(nonatomic, strong) UILabel *maxCameraValue;
@property(nonatomic, strong) UILabel *minCameraValue;
@property(nonatomic, strong) UILabel *cameraPitchValue;
@property(nonatomic, strong) UILabel *scrollSpeedValue;
@property(nonatomic, strong) UILabel *drawDistanceValue;
@property(nonatomic, strong) UILabel *fpsValue;
@property(nonatomic, strong) UISwitch *enforceMaxSwitch;
@property(nonatomic, strong) UISwitch *fpsLimitSwitch;

@property(nonatomic, strong) UISegmentedControl *zeroHourControlBarSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourCameosSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourMusicSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourVoicesSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourHotkeysSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourHotkeyLanguageSegment;
@property(nonatomic, strong) UISegmentedControl *zeroHourPortraitsSegment;
@property(nonatomic, strong) UISwitch *zeroHourFogSwitch;
@property(nonatomic, strong) UISwitch *zeroHourWaterSwitch;
@property(nonatomic, strong) UISwitch *zeroHourExtraBuildingPropsSwitch;

@property(nonatomic, strong) UISwitch *shadow3DSwitch;
@property(nonatomic, strong) UISwitch *shadow2DSwitch;
@property(nonatomic, strong) UISwitch *cloudShadowsSwitch;
@property(nonatomic, strong) UISwitch *groundLightingSwitch;
@property(nonatomic, strong) UISwitch *softWaterSwitch;
@property(nonatomic, strong) UISwitch *buildingOcclusionSwitch;
@property(nonatomic, strong) UISwitch *showPropsSwitch;
@property(nonatomic, strong) UISwitch *extraAnimationsSwitch;
@property(nonatomic, strong) UISwitch *dynamicLODSwitch;
@property(nonatomic, strong) UISwitch *heatEffectsSwitch;
@property(nonatomic, strong) UISegmentedControl *textureQualitySegment;
@property(nonatomic, strong) UISegmentedControl *particleQualitySegment;
@property(nonatomic, strong) UISegmentedControl *textureFilterSegment;
@property(nonatomic, strong) UISegmentedControl *anisotropySegment;
@property(nonatomic, strong) UISegmentedControl *antiAliasingSegment;

@property(nonatomic, strong) UIView *diagnosticsView;
@property(nonatomic, strong) UIView *modalBackdrop;
@property(nonatomic, strong) UIView *profileView;
@property(nonatomic, strong) UILabel *diagnosticsText;
@property(nonatomic, strong) UIView *gameFileView;
@property(nonatomic, strong) UIProgressView *gameFileProgress;
@property(nonatomic, strong) UILabel *gameFileStage;
@property(nonatomic, strong) UILabel *gameFileDetail;
@property(nonatomic, strong) UIButton *gameFileCancelButton;
@property(nonatomic, strong) UIButton *gameFileReinstallButton;
@property(nonatomic, strong) UIButton *gameFileMinimizeButton;
@property(nonatomic, strong) UIButton *gameFileCloseButton;
@property(nonatomic, strong) UILabel *gameFilePercentLabel;
@property(nonatomic, strong) UIButton *shareDiagnosticsButton;
@property(nonatomic, strong) UIButton *gameFileStatusButton;
@property(nonatomic, assign) BOOL diagnosticsScanRunning;
@property(nonatomic, strong) AVPlayer *gameFileVideoPlayer;
@property(nonatomic, strong) AVPlayerLayer *gameFileVideoLayer;
@property(nonatomic, strong) NSMutableArray<UIVisualEffectView *> *gameFileEdgeBlurViews;
@property(nonatomic, strong) id gameFileVideoLoopObserver;

- (void)resetНастройкиControls;
@end

void GeneralsXSetIOSDiagnosticClearCallback(GeneralsXIOSDiagnosticClearCallback callback)
{
    gDiagnosticClearCallback = callback;
}

@implementation GXProfileLauncherViewController

- (void)viewDidLoad
{
    [super viewDidLoad];

    self.view.backgroundColor = UIColor.blackColor;
    EnsureDefaultIOSIPadOverrides();
    EnsureDefaultZeroHourSettings();

    [self buildMenu];

    self.modalBackdrop = [[UIView alloc] init];
    self.modalBackdrop.translatesAutoresizingMaskIntoConstraints = NO;
    self.modalBackdrop.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.48];
    self.modalBackdrop.hidden = YES;
    [self.view addSubview:self.modalBackdrop];
    MakeGlassBlurView(self.modalBackdrop);
    [NSLayoutConstraint activateConstraints:@[
        [self.modalBackdrop.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.modalBackdrop.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.modalBackdrop.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.modalBackdrop.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    [self buildSettings];
    [self buildDiagnostics];
    [self buildProfileModal];
}

- (void)buildMenu
{
    NSString *bundledProfile = BundledAutoLaunchProfile();
    BOOL dedicatedZeroHour = [bundledProfile isEqualToString:@"zerohour"];

    self.view.backgroundColor = [UIColor colorWithRed:0.008 green:0.018 blue:0.035 alpha:1.0];

    UIScrollView *scrollView = [[UIScrollView alloc] init];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.scrollEnabled = NO;
    scrollView.alwaysBounceVertical = NO;
    scrollView.showsVerticalScrollIndicator = NO;
    scrollView.backgroundColor = UIColor.clearColor;

    UIStackView *content = [[UIStackView alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 5.0;
    content.layoutMargins = UIEdgeInsetsMake(4.0, 10.0, 5.0, 10.0);
    content.layoutMarginsRelativeArrangement = YES;

    UIView *header = [[UIView alloc] init];
    header.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel *title = MakeLabel(@"Generals: Zero Hour", 23.0, UIFontWeightBold);
    title.textAlignment = NSTextAlignmentLeft;
    [header addSubview:title];

    UILabel *subtitle = MakeLabel(@"iOS / iPad Launcher", 13.0, UIFontWeightRegular);
    subtitle.textAlignment = NSTextAlignmentLeft;
    subtitle.textColor = [UIColor colorWithRed:0.38 green:0.72 blue:1.0 alpha:1.0];
    [header addSubview:subtitle];

    UILabel *platform = MakeLabel(@"  iPhone / iPad\niOS", 12.0, UIFontWeightSemibold);
    platform.textAlignment = NSTextAlignmentCenter;
    platform.textColor = [UIColor colorWithWhite:0.88 alpha:1.0];
    platform.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.82];
    platform.layer.cornerRadius = 18.0;
    platform.layer.borderWidth = 1.0;
    platform.layer.borderColor = [UIColor colorWithRed:0.20 green:0.45 blue:0.72 alpha:0.8].CGColor;
    platform.clipsToBounds = YES;
    [header addSubview:platform];

    // Header must have explicit constraints. Without them Auto Layout can
    // collapse/overlap the title, iOS/iPad subtitle and device pill.
    [NSLayoutConstraint activateConstraints:@[
        [header.heightAnchor constraintEqualToConstant:44.0],
        [title.leadingAnchor constraintEqualToAnchor:header.leadingAnchor],
        [title.topAnchor constraintEqualToAnchor:header.topAnchor constant:2.0],
        [title.trailingAnchor constraintLessThanOrEqualToAnchor:platform.leadingAnchor constant:-18.0],
        [subtitle.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [subtitle.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:2.0],
        [subtitle.trailingAnchor constraintLessThanOrEqualToAnchor:platform.leadingAnchor constant:-18.0],
        [platform.trailingAnchor constraintEqualToAnchor:header.trailingAnchor],
        [platform.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],
        [platform.widthAnchor constraintEqualToConstant:124.0],
        [platform.heightAnchor constraintEqualToConstant:38.0]
    ]];

    [content addArrangedSubview:header];

    UIView *hero = [[UIView alloc] init];
    hero.translatesAutoresizingMaskIntoConstraints = NO;
    hero.backgroundColor = [UIColor colorWithRed:0.035 green:0.085 blue:0.14 alpha:1.0];
    hero.layer.cornerRadius = 24.0;
    hero.layer.borderWidth = 1.0;
    hero.layer.borderColor = [UIColor colorWithRed:0.16 green:0.40 blue:0.65 alpha:0.85].CGColor;
    hero.clipsToBounds = YES;
    UIImageView *heroImage = [[UIImageView alloc] init];
    heroImage.translatesAutoresizingMaskIntoConstraints = NO;
    heroImage.contentMode = UIViewContentModeScaleAspectFill;
    heroImage.clipsToBounds = YES;
    heroImage.alpha = 0.94;
    [hero addSubview:heroImage];

    [NSLayoutConstraint activateConstraints:@[
        [heroImage.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor],
        [heroImage.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor],
        [heroImage.topAnchor constraintEqualToAnchor:hero.topAnchor],
        [heroImage.bottomAnchor constraintEqualToAnchor:hero.bottomAnchor]
    ]];

    CAGradientLayer *heroGradient = [CAGradientLayer layer];
    heroGradient.frame = CGRectMake(0, 0, 1000, 420);
    heroGradient.colors = @[
        (id)[UIColor colorWithRed:0.03 green:0.10 blue:0.18 alpha:0.42].CGColor,
        (id)[UIColor colorWithRed:0.01 green:0.025 blue:0.055 alpha:0.72].CGColor
    ];
    heroGradient.startPoint = CGPointMake(0.0, 0.0);
    heroGradient.endPoint = CGPointMake(1.0, 1.0);
    [hero.layer addSublayer:heroGradient];
    heroGradient.frame = hero.bounds;

    NSURL *heroURL = [NSURL URLWithString:@"https://media.contentapi.ea.com/content/dam/gin/images/2017/01/command-and-conquer-generals-zero-hour-key-art.jpg"];
    if (heroURL != nil)
    {
        [[[NSURLSession sharedSession] dataTaskWithURL:heroURL
                                    completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            if (data.length == 0 || error != nil)
                return;
            UIImage *image = [UIImage imageWithData:data];
            if (image == nil)
                return;
            dispatch_async(dispatch_get_main_queue(), ^{
                heroImage.image = image;
            });
        }] resume];
    }

    UILabel *gameTitle = MakeLabel(@"COMMAND & CONQUER", 20.0, UIFontWeightBold);
    gameTitle.textAlignment = NSTextAlignmentLeft;
    gameTitle.textColor = [UIColor colorWithWhite:0.93 alpha:1.0];
    [hero addSubview:gameTitle];

    UILabel *zeroHour = MakeLabel(@"GENERALS\nZERO HOUR", 42.0, UIFontWeightBlack);
    zeroHour.textAlignment = NSTextAlignmentLeft;
    zeroHour.textColor = UIColor.whiteColor;
    [hero addSubview:zeroHour];

    UILabel *tagline = MakeLabel(@"Нативный лаунчер от spideywv", 13.0, UIFontWeightRegular);
    tagline.textAlignment = NSTextAlignmentLeft;
    tagline.textColor = [UIColor colorWithWhite:0.65 alpha:1.0];
    [hero addSubview:tagline];

    UIButton *play = MakeButton(@"▶   ИГРАТЬ   ›", self, @selector(launchVanilla));
    play.titleLabel.font = [UIFont systemFontOfSize:25.0 weight:UIFontWeightBold];
    play.backgroundColor = [UIColor colorWithRed:0.03 green:0.43 blue:0.95 alpha:1.0];
    play.layer.cornerRadius = 30.0;
    play.layer.borderWidth = 1.0;
    play.layer.borderColor = [UIColor colorWithRed:0.35 green:0.80 blue:1.0 alpha:0.95].CGColor;
    play.layer.shadowColor = [UIColor colorWithRed:0.0 green:0.45 blue:1.0 alpha:1.0].CGColor;
    play.layer.shadowOpacity = 0.55;
    play.layer.shadowRadius = 18.0;
    play.layer.shadowOffset = CGSizeMake(0, 5);
    [hero addSubview:play];

    UILabel *heroHint = MakeLabel(dedicatedZeroHour
                                      ? @"Профиль: Zero Hour"
                                      : @"Профиль: vanilla",
                                  12.0,
                                  UIFontWeightSemibold);
    heroHint.textAlignment = NSTextAlignmentRight;
    heroHint.textColor = [UIColor colorWithRed:0.35 green:0.82 blue:1.0 alpha:1.0];
    [hero addSubview:heroHint];

    [NSLayoutConstraint activateConstraints:@[
        // Fixed hero height: loading the remote artwork must never resize the launcher.
        [hero.heightAnchor constraintEqualToConstant:172.0],

        [gameTitle.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:28.0],
        [gameTitle.topAnchor constraintEqualToAnchor:hero.topAnchor constant:24.0],

        [zeroHour.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:28.0],
        [zeroHour.topAnchor constraintEqualToAnchor:gameTitle.bottomAnchor constant:4.0],

        [tagline.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:28.0],
        [tagline.topAnchor constraintEqualToAnchor:zeroHour.bottomAnchor constant:8.0],

        [play.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:28.0],
        [play.bottomAnchor constraintEqualToAnchor:hero.bottomAnchor constant:-28.0],
        [play.widthAnchor constraintEqualToConstant:250.0],
        [play.heightAnchor constraintEqualToConstant:46.0],

        [heroHint.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor constant:-28.0],
        [heroHint.centerYAnchor constraintEqualToAnchor:play.centerYAnchor],

        [hero.heightAnchor constraintGreaterThanOrEqualToConstant:172.0]
    ]];

    [content addArrangedSubview:hero];

    UIView *sidePanel = [[UIView alloc] init];
    sidePanel.translatesAutoresizingMaskIntoConstraints = NO;
    sidePanel.backgroundColor = UIColor.clearColor;

    UIStackView *sideStack = [[UIStackView alloc] init];
    sideStack.translatesAutoresizingMaskIntoConstraints = NO;
    sideStack.axis = UILayoutConstraintAxisVertical;
    sideStack.spacing = 12.0;

    self.gameFileStatusButton = [self makeLauncherCard:@"СТАТУС ФАЙЛА ИГРЫ"
                                        subtitle:@"Проверка…"
                                           icon:@"checkmark.circle.fill"
                                          action:@selector(downloadGameFile)
                                      accentColor:[UIColor colorWithRed:0.18 green:0.88 blue:0.48 alpha:1.0]];
    [sideStack addArrangedSubview:self.gameFileStatusButton];
    [sidePanel addSubview:sideStack];

    [NSLayoutConstraint activateConstraints:@[
        [sideStack.leadingAnchor constraintEqualToAnchor:sidePanel.leadingAnchor],
        [sideStack.trailingAnchor constraintEqualToAnchor:sidePanel.trailingAnchor],
        [sideStack.topAnchor constraintEqualToAnchor:sidePanel.topAnchor],
        [sideStack.bottomAnchor constraintEqualToAnchor:sidePanel.bottomAnchor]
    ]];

    UIStackView *mainColumns = [[UIStackView alloc] init];
    mainColumns.translatesAutoresizingMaskIntoConstraints = NO;
    mainColumns.axis = UILayoutConstraintAxisHorizontal;
    mainColumns.spacing = 14.0;
    mainColumns.alignment = UIStackViewAlignmentFill;
    [mainColumns addArrangedSubview:hero];
    [mainColumns addArrangedSubview:sidePanel];

    // Re-parent hero from the vertical content stack into the adaptive columns.
    [content removeArrangedSubview:hero];

    UIView *mainSection = [[UIView alloc] init];
    mainSection.translatesAutoresizingMaskIntoConstraints = NO;
    [mainSection addSubview:mainColumns];

    [NSLayoutConstraint activateConstraints:@[
        [mainColumns.leadingAnchor constraintEqualToAnchor:mainSection.leadingAnchor],
        [mainColumns.trailingAnchor constraintEqualToAnchor:mainSection.trailingAnchor],
        [mainColumns.topAnchor constraintEqualToAnchor:mainSection.topAnchor],
        [mainColumns.bottomAnchor constraintEqualToAnchor:mainSection.bottomAnchor],
        [sidePanel.widthAnchor constraintGreaterThanOrEqualToConstant:250.0]
    ]];

    [content addArrangedSubview:mainSection];

    UIStackView *actions = [[UIStackView alloc] init];
    actions.translatesAutoresizingMaskIntoConstraints = NO;
    actions.axis = UILayoutConstraintAxisHorizontal;
    actions.spacing = 12.0;
    actions.distribution = UIStackViewDistributionFillEqually;

    UIButton *gameFileAction = [self makeLauncherCard:@"Файл игры"
                                              subtitle:@"Файл игры и установка"
                                                 icon:@"arrow.down.circle.fill"
                                                action:@selector(downloadGameFile)
                                            accentColor:[UIColor colorWithRed:0.30 green:0.72 blue:1.0 alpha:1.0]];
    UIButton *settingsAction = [self makeLauncherCard:@"Настройки"
                                              subtitle:@"Графика, звук, управление"
                                                 icon:@"gearshape.fill"
                                                action:@selector(showНастройки)
                                            accentColor:[UIColor colorWithRed:0.55 green:0.68 blue:0.95 alpha:1.0]];
    UIButton *diagnosticsAction = [self makeLauncherCard:@"Диагностика"
                                                 subtitle:@"Логи, информация, проверка"
                                                    icon:@"stethoscope"
                                                   action:@selector(showDiagnostics)
                                               accentColor:[UIColor colorWithRed:0.38 green:0.90 blue:0.72 alpha:1.0]];

    [actions addArrangedSubview:gameFileAction];
    [actions addArrangedSubview:settingsAction];
    [actions addArrangedSubview:diagnosticsAction];
    [content addArrangedSubview:actions];

    UIView *systemCard = [[UIView alloc] init];
    systemCard.translatesAutoresizingMaskIntoConstraints = NO;
    systemCard.backgroundColor = [UIColor colorWithWhite:0.045 alpha:0.96];
    systemCard.layer.cornerRadius = 18.0;
    systemCard.layer.borderWidth = 1.0;
    systemCard.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.10].CGColor;

    UILabel *systemTitle = MakeLabel(@"СИСТЕМА", 16.0, UIFontWeightBold);
    systemTitle.textAlignment = NSTextAlignmentLeft;
    [systemCard addSubview:systemTitle];

    UILabel *systemText = MakeLabel(@"Vulkan / DXVK / MoltenVK\nNative iOS launcher\n1792 × 828 • iPhone / iPad", 13.0, UIFontWeightRegular);
    systemText.textAlignment = NSTextAlignmentLeft;
    systemText.textColor = [UIColor colorWithWhite:0.68 alpha:1.0];
    [systemCard addSubview:systemText];

    UILabel *ready = MakeLabel(@"●  LAUNCHER READY", 12.0, UIFontWeightBold);
    ready.textAlignment = NSTextAlignmentRight;
    ready.textColor = [UIColor colorWithRed:0.18 green:0.90 blue:0.50 alpha:1.0];
    [systemCard addSubview:ready];

    [NSLayoutConstraint activateConstraints:@[
        [systemTitle.leadingAnchor constraintEqualToAnchor:systemCard.leadingAnchor constant:20.0],
        [systemTitle.topAnchor constraintEqualToAnchor:systemCard.topAnchor constant:17.0],
        [systemText.leadingAnchor constraintEqualToAnchor:systemTitle.leadingAnchor],
        [systemText.topAnchor constraintEqualToAnchor:systemTitle.bottomAnchor constant:7.0],
        [systemText.bottomAnchor constraintEqualToAnchor:systemCard.bottomAnchor constant:-17.0],
        [ready.trailingAnchor constraintEqualToAnchor:systemCard.trailingAnchor constant:-20.0],
        [ready.centerYAnchor constraintEqualToAnchor:systemCard.centerYAnchor]
    ]];

    [content addArrangedSubview:systemCard];

    [scrollView addSubview:content];
    [self.view addSubview:scrollView];

    self.menuStack = content;

    [NSLayoutConstraint activateConstraints:@[
        [scrollView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [scrollView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],

        [content.leadingAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.leadingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.topAnchor],
        [content.bottomAnchor constraintEqualToAnchor:scrollView.contentLayoutGuide.bottomAnchor],
        [content.widthAnchor constraintEqualToAnchor:scrollView.frameLayoutGuide.widthAnchor]
    ]];

    if (@available(iOS 16.0, *))
    {
        mainColumns.axis = UILayoutConstraintAxisHorizontal;
    }

    [self adaptModernLauncherLayout:mainColumns hero:hero sidePanel:sidePanel actions:actions];
    [self refreshGameFileStatusCard];
}

- (UIButton *)makeLauncherCard:(NSString *)title
                      subtitle:(NSString *)subtitle
                          icon:(NSString *)iconName
                         action:(SEL)action
                    accentColor:(UIColor *)accentColor
{
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.backgroundColor = [UIColor colorWithWhite:0.06 alpha:0.42];
    button.layer.shadowColor = accentColor.CGColor;
    button.layer.shadowOpacity = 0.18;
    button.layer.shadowRadius = 14.0;
    button.layer.shadowOffset = CGSizeMake(0, 6);
    MakeGlassBlurView(button);
    button.layer.cornerRadius = 18.0;
    button.layer.borderWidth = 1.0;
    button.layer.borderColor = [UIColor colorWithRed:0.12 green:0.30 blue:0.50 alpha:0.75].CGColor;
    button.contentEdgeInsets = UIEdgeInsetsMake(8.0, 12.0, 8.0, 12.0);

    UIImage *image = [UIImage systemImageNamed:iconName];
    if (image != nil)
    {
        image = [image imageWithTintColor:accentColor renderingMode:UIImageRenderingModeAlwaysOriginal];
        [button setImage:image forState:UIControlStateNormal];
        button.imageView.contentMode = UIViewContentModeScaleAspectFit;
    }

    NSString *display = [NSString stringWithFormat:@"%@\n%@", title, subtitle];
    NSMutableAttributedString *attributed = [[NSMutableAttributedString alloc] initWithString:display];
    [attributed addAttribute:NSFontAttributeName
                       value:[UIFont systemFontOfSize:15.0 weight:UIFontWeightBold]
                       range:NSMakeRange(0, title.length)];
    [attributed addAttribute:NSForegroundColorAttributeName
                       value:UIColor.whiteColor
                       range:NSMakeRange(0, title.length)];
    NSRange subtitleRange = NSMakeRange(title.length + 1, subtitle.length);
    [attributed addAttribute:NSFontAttributeName
                       value:[UIFont systemFontOfSize:11.0 weight:UIFontWeightRegular]
                       range:subtitleRange];
    [attributed addAttribute:NSForegroundColorAttributeName
                       value:[UIColor colorWithWhite:0.62 alpha:1.0]
                       range:subtitleRange];

    [button setAttributedTitle:attributed forState:UIControlStateNormal];
    button.titleLabel.numberOfLines = 2;
    button.titleLabel.textAlignment = NSTextAlignmentLeft;
    button.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    if (action != NULL)
        [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    else
        button.userInteractionEnabled = NO;
    [button.heightAnchor constraintEqualToConstant:72.0].active = YES;

    return button;
}

- (void)adaptModernLauncherLayout:(UIStackView *)columns
                              hero:(UIView *)hero
                        sidePanel:(UIView *)sidePanel
                           actions:(UIStackView *)actions
{
    void (^update)(void) = ^{
        BOOL compactWidth = self.view.bounds.size.width < 700.0;
        BOOL shortLandscape = self.view.bounds.size.height < 500.0;

        // Non-scrollable landscape launcher: all primary controls fit in the viewport.
        columns.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
        columns.spacing = shortLandscape ? 6.0 : (compactWidth ? 10.0 : 12.0);
        actions.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
        actions.spacing = shortLandscape ? 6.0 : 10.0;

        if (!compactWidth)
        {
            [sidePanel.widthAnchor constraintEqualToConstant:(shortLandscape ? 170.0 : 190.0)].active = YES;
            [hero.heightAnchor constraintEqualToConstant:(shortLandscape ? 138.0 : 172.0)].active = YES;
        }
        else
        {
            [hero.heightAnchor constraintEqualToConstant:(shortLandscape ? 120.0 : 172.0)].active = YES;
        }
    };

    update();
}

- (UISlider *)makeSliderWithMin:(float)minimum max:(float)maximum
{
    UISlider *slider = [[UISlider alloc] init];
    slider.translatesAutoresizingMaskIntoConstraints = NO;
    slider.minimumValue = minimum;
    slider.maximumValue = maximum;
    slider.minimumTrackTintColor = UIColor.whiteColor;
    slider.maximumTrackTintColor = [UIColor colorWithWhite:0.25 alpha:1.0];
    [slider addTarget:self action:@selector(settingsSliderChanged:) forControlEvents:UIControlEventValueChanged];
    return slider;
}

- (UILabel *)makeValueLabel
{
    UILabel *label = MakeLabel(@"", 15.0, UIFontWeightSemibold);
    label.textAlignment = NSTextAlignmentRight;
    label.font = [UIFont monospacedDigitSystemFontOfSize:15.0 weight:UIFontWeightSemibold];
    [label.widthAnchor constraintEqualToConstant:72.0].active = YES;
    return label;
}

- (UIStackView *)sliderRow:(NSString *)title slider:(UISlider *)slider value:(UILabel *)value
{
    UILabel *name = MakeLabel(title, 15.0, UIFontWeightMedium);
    name.textAlignment = NSTextAlignmentLeft;

    UIStackView *line = [[UIStackView alloc] initWithArrangedSubviews:@[slider, value]];
    line.axis = UILayoutConstraintAxisHorizontal;
    line.alignment = UIStackViewAlignmentCenter;
    line.spacing = 14.0;

    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[name, line]];
    row.axis = UILayoutConstraintAxisVertical;
    row.alignment = UIStackViewAlignmentFill;
    row.spacing = 7.0;
    row.layoutMargins = UIEdgeInsetsMake(10.0, 14.0, 10.0, 14.0);
    row.layoutMarginsRelativeArrangement = YES;
    row.backgroundColor = [UIColor colorWithWhite:0.055 alpha:1.0];
    row.layer.cornerRadius = 9.0;
    return row;
}

- (UIStackView *)switchRow:(NSString *)title control:(UISwitch *)control
{
    UILabel *name = MakeLabel(title, 15.0, UIFontWeightMedium);
    name.textAlignment = NSTextAlignmentLeft;

    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[name, control]];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.distribution = UIStackViewDistributionFill;
    row.spacing = 18.0;
    row.layoutMargins = UIEdgeInsetsMake(10.0, 14.0, 10.0, 14.0);
    row.layoutMarginsRelativeArrangement = YES;
    row.backgroundColor = [UIColor colorWithWhite:0.055 alpha:1.0];
    row.layer.cornerRadius = 9.0;
    return row;
}

- (UILabel *)sectionLabel:(NSString *)title
{
    UILabel *label = MakeLabel(title, 13.0, UIFontWeightBold);
    label.textAlignment = NSTextAlignmentLeft;
    label.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
    return label;
}

- (UISegmentedControl *)makeSegmented:(NSArray<NSString *> *)items
{
    UISegmentedControl *control = [[UISegmentedControl alloc] initWithItems:items];
    control.translatesAutoresizingMaskIntoConstraints = NO;
    control.selectedSegmentIndex = 0;
    return control;
}

- (UIStackView *)segmentedRow:(NSString *)title control:(UISegmentedControl *)control
{
    UILabel *name = MakeLabel(title, 15.0, UIFontWeightMedium);
    name.textAlignment = NSTextAlignmentLeft;

    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[name, control]];
    row.axis = UILayoutConstraintAxisVertical;
    row.alignment = UIStackViewAlignmentFill;
    row.spacing = 8.0;
    row.layoutMargins = UIEdgeInsetsMake(10.0, 14.0, 10.0, 14.0);
    row.layoutMarginsRelativeArrangement = YES;
    row.backgroundColor = [UIColor colorWithWhite:0.055 alpha:1.0];
    row.layer.cornerRadius = 9.0;
    return row;
}

- (void)buildSettings
{
    self.settingsView = [[UIView alloc] init];
    self.settingsView.translatesAutoresizingMaskIntoConstraints = NO;
    self.settingsView.backgroundColor = [UIColor colorWithWhite:0.02 alpha:0.56];
    self.settingsView.layer.cornerRadius = 28.0;
    self.settingsView.layer.borderWidth = 1.0;
    self.settingsView.layer.borderColor = [UIColor colorWithRed:0.25 green:0.60 blue:1.0 alpha:0.45].CGColor;
    self.settingsView.layer.shadowColor = UIColor.blackColor.CGColor;
    self.settingsView.layer.shadowOpacity = 0.45;
    self.settingsView.layer.shadowRadius = 28.0;
    self.settingsView.layer.shadowOffset = CGSizeMake(0, 12);
    MakeGlassBlurView(self.settingsView);
    self.settingsView.hidden = YES;
    [self.view addSubview:self.settingsView];

    [NSLayoutConstraint activateConstraints:@[
        [self.settingsView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:28.0],
        [self.settingsView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-28.0],
        [self.settingsView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:18.0],
        [self.settingsView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-18.0],
    ]];

    UILabel *title = MakeLabel(@"Настройки ZeroHour", 26.0, UIFontWeightBold);
    title.textAlignment = NSTextAlignmentLeft;

    UILabel *note = MakeLabel(@"Эквиваленты настроек официального лаунчера ZeroHour для iOS/iPad. Изменения применяются при следующем запуске игры.", 13.0, UIFontWeightRegular);
    note.textAlignment = NSTextAlignmentLeft;
    note.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];

    self.zeroHourControlBarSegment = [self makeSegmented:@[@"ZeroHour", @"Pro", @"Standard"]];
    self.zeroHourCameosSegment = [self makeSegmented:@[@"Standard", @"HD"]];
    self.zeroHourMusicSegment = [self makeSegmented:@[@"Standard", @"Enhanced", @"The Score"]];
    self.zeroHourVoicesSegment = [self makeSegmented:@[@"English", @"Native"]];
    self.zeroHourHotkeysSegment = [self makeSegmented:@[@"Original", @"Leikeze"]];
    self.zeroHourHotkeyLanguageSegment = [self makeSegmented:@[@"English", @"Russian"]];
    self.zeroHourPortraitsSegment = [self makeSegmented:@[@"Standard", @"Funny"]];
    self.zeroHourFogSwitch = [[UISwitch alloc] init];
    self.zeroHourWaterSwitch = [[UISwitch alloc] init];
    self.zeroHourExtraBuildingPropsSwitch = [[UISwitch alloc] init];

    self.shadow3DSwitch = [[UISwitch alloc] init];
    self.shadow2DSwitch = [[UISwitch alloc] init];
    self.cloudShadowsSwitch = [[UISwitch alloc] init];
    self.groundLightingSwitch = [[UISwitch alloc] init];
    self.softWaterSwitch = [[UISwitch alloc] init];
    self.buildingOcclusionSwitch = [[UISwitch alloc] init];
    self.showPropsSwitch = [[UISwitch alloc] init];
    self.extraAnimationsSwitch = [[UISwitch alloc] init];
    self.dynamicLODSwitch = [[UISwitch alloc] init];
    self.heatEffectsSwitch = [[UISwitch alloc] init];
    self.textureQualitySegment = [self makeSegmented:@[@"High", @"Medium", @"Low"]];
    self.particleQualitySegment = [self makeSegmented:@[@"Low", @"Medium", @"High"]];
    self.textureFilterSegment = [self makeSegmented:@[@"Bilinear", @"Trilinear", @"Anisotropic"]];
    self.anisotropySegment = [self makeSegmented:@[@"2x", @"4x", @"8x", @"16x"]];
    self.antiAliasingSegment = [self makeSegmented:@[@"Off", @"2x", @"4x", @"8x"]];
    [self.zeroHourControlBarSegment setTitle:@"ZeroHour" forSegmentAtIndex:0];
    [self.zeroHourControlBarSegment setTitle:@"Про" forSegmentAtIndex:1];
    [self.zeroHourControlBarSegment setTitle:@"Стандарт" forSegmentAtIndex:2];
    [self.zeroHourCameosSegment setTitle:@"Стандарт" forSegmentAtIndex:0];
    [self.zeroHourCameosSegment setTitle:@"HD" forSegmentAtIndex:1];
    [self.zeroHourMusicSegment setTitle:@"Стандарт" forSegmentAtIndex:0];
    [self.zeroHourMusicSegment setTitle:@"Улучшенная" forSegmentAtIndex:1];
    [self.zeroHourMusicSegment setTitle:@"The Score" forSegmentAtIndex:2];
    [self.zeroHourVoicesSegment setTitle:@"Английский" forSegmentAtIndex:0];
    [self.zeroHourVoicesSegment setTitle:@"Родной" forSegmentAtIndex:1];
    [self.zeroHourHotkeysSegment setTitle:@"Оригинал" forSegmentAtIndex:0];
    [self.zeroHourHotkeysSegment setTitle:@"Leikeze" forSegmentAtIndex:1];
    [self.zeroHourHotkeyLanguageSegment setTitle:@"Английский" forSegmentAtIndex:0];
    [self.zeroHourHotkeyLanguageSegment setTitle:@"Русский" forSegmentAtIndex:1];
    [self.zeroHourPortraitsSegment setTitle:@"Стандарт" forSegmentAtIndex:0];
    [self.zeroHourPortraitsSegment setTitle:@"Смешные" forSegmentAtIndex:1];
    [self.textureQualitySegment setTitle:@"Высокое" forSegmentAtIndex:0];
    [self.textureQualitySegment setTitle:@"Среднее" forSegmentAtIndex:1];
    [self.textureQualitySegment setTitle:@"Низкое" forSegmentAtIndex:2];
    [self.particleQualitySegment setTitle:@"Низкое" forSegmentAtIndex:0];
    [self.particleQualitySegment setTitle:@"Среднее" forSegmentAtIndex:1];
    [self.particleQualitySegment setTitle:@"Высокое" forSegmentAtIndex:2];
    [self.textureFilterSegment setTitle:@"Билинейная" forSegmentAtIndex:0];
    [self.textureFilterSegment setTitle:@"Трилинейная" forSegmentAtIndex:1];
    [self.textureFilterSegment setTitle:@"Анизотропная" forSegmentAtIndex:2];
    [self.anisotropySegment setTitle:@"2x" forSegmentAtIndex:0];
    [self.anisotropySegment setTitle:@"4x" forSegmentAtIndex:1];
    [self.anisotropySegment setTitle:@"8x" forSegmentAtIndex:2];
    [self.anisotropySegment setTitle:@"16x" forSegmentAtIndex:3];
    [self.antiAliasingSegment setTitle:@"Выкл." forSegmentAtIndex:0];
    [self.antiAliasingSegment setTitle:@"2x" forSegmentAtIndex:1];
    [self.antiAliasingSegment setTitle:@"4x" forSegmentAtIndex:2];
    [self.antiAliasingSegment setTitle:@"8x" forSegmentAtIndex:3];

    self.maxCameraSlider = [self makeSliderWithMin:300.0f max:800.0f];
    self.minCameraSlider = [self makeSliderWithMin:40.0f max:150.0f];
    self.cameraPitchSlider = [self makeSliderWithMin:20.0f max:60.0f];
    self.scrollSpeedSlider = [self makeSliderWithMin:0.5f max:2.0f];
    self.drawDistanceSlider = [self makeSliderWithMin:0.5f max:2.0f];
    self.fpsSlider = [self makeSliderWithMin:30.0f max:120.0f];

    self.maxCameraValue = [self makeValueLabel];
    self.minCameraValue = [self makeValueLabel];
    self.cameraPitchValue = [self makeValueLabel];
    self.scrollSpeedValue = [self makeValueLabel];
    self.drawDistanceValue = [self makeValueLabel];
    self.fpsValue = [self makeValueLabel];

    self.enforceMaxSwitch = [[UISwitch alloc] init];
    self.fpsLimitSwitch = [[UISwitch alloc] init];
    [self.fpsLimitSwitch addTarget:self action:@selector(fpsLimitChanged:) forControlEvents:UIControlEventValueChanged];

    UIStackView *controls = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self sectionLabel:@"ZERO HOUR"],
        [self segmentedRow:@"Панель управления" control:self.zeroHourControlBarSegment],
        [self segmentedRow:@"Качество иконок / портретов" control:self.zeroHourCameosSegment],
        [self segmentedRow:@"Music" control:self.zeroHourMusicSegment],
        [self segmentedRow:@"Голоса юнитов" control:self.zeroHourVoicesSegment],
        [self segmentedRow:@"Hotkeys" control:self.zeroHourHotkeysSegment],
        [self segmentedRow:@"Язык горячих клавиш" control:self.zeroHourHotkeyLanguageSegment],
        [self segmentedRow:@"Портреты генералов" control:self.zeroHourPortraitsSegment],
        [self switchRow:@"Эффекты тумана" control:self.zeroHourFogSwitch],
        [self switchRow:@"Эффекты воды" control:self.zeroHourWaterSwitch],
        [self switchRow:@"Дополнительные элементы зданий" control:self.zeroHourExtraBuildingPropsSwitch],

        [self sectionLabel:@"ГРАФИКА"],
        [self switchRow:@"3D-тени" control:self.shadow3DSwitch],
        [self switchRow:@"2D-тени" control:self.shadow2DSwitch],
        [self switchRow:@"Тени от облаков" control:self.cloudShadowsSwitch],
        [self switchRow:@"Освещение поверхности" control:self.groundLightingSwitch],
        [self switchRow:@"Плавные границы воды" control:self.softWaterSwitch],
        [self switchRow:@"Юниты за зданиями" control:self.buildingOcclusionSwitch],
        [self switchRow:@"Мелкие объекты / деревья" control:self.showPropsSwitch],
        [self switchRow:@"Дополнительные анимации" control:self.extraAnimationsSwitch],
        [self switchRow:@"Динамический LOD" control:self.dynamicLODSwitch],
        [self switchRow:@"Эффекты нагрева" control:self.heatEffectsSwitch],
        [self segmentedRow:@"Качество текстур" control:self.textureQualitySegment],
        [self segmentedRow:@"Частицы" control:self.particleQualitySegment],
        [self segmentedRow:@"Фильтрация текстур" control:self.textureFilterSegment],
        [self segmentedRow:@"Анизотропная фильтрация" control:self.anisotropySegment],
        [self segmentedRow:@"Сглаживание MSAA" control:self.antiAliasingSegment],

        [self sectionLabel:@"КАМЕРА / ПРОИЗВОДИТЕЛЬНОСТЬ"],
        [self sliderRow:@"Максимальная высота камеры" slider:self.maxCameraSlider value:self.maxCameraValue],
        [self sliderRow:@"Минимальная высота камеры" slider:self.minCameraSlider value:self.minCameraValue],
        [self sliderRow:@"Наклон камеры" slider:self.cameraPitchSlider value:self.cameraPitchValue],
        [self switchRow:@"Ограничить максимальную высоту камеры" control:self.enforceMaxSwitch],
        [self sliderRow:@"Скорость прокрутки клавиатурой / у края экрана" slider:self.scrollSpeedSlider value:self.scrollSpeedValue],
        [self sliderRow:@"Дальность отрисовки ландшафта" slider:self.drawDistanceSlider value:self.drawDistanceValue],
        [self switchRow:@"Ограничение FPS" control:self.fpsLimitSwitch],
        [self sliderRow:@"Кадры в секунду" slider:self.fpsSlider value:self.fpsValue],
    ]];
    controls.translatesAutoresizingMaskIntoConstraints = NO;
    controls.axis = UILayoutConstraintAxisVertical;
    controls.alignment = UIStackViewAlignmentFill;
    controls.spacing = 9.0;

    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = YES;
    scroll.showsVerticalScrollIndicator = YES;
    [scroll addSubview:controls];

    UIButton *save = MakeButton(@"Сохранить", self, @selector(saveНастройки));
    UIButton *reset = MakeButton(@"Сбросить настройки", self, @selector(resetНастройки));
    UIButton *back = MakeButton(@"Назад", self, @selector(hideНастройки));

    [save.widthAnchor constraintEqualToConstant:180.0].active = YES;
    [reset.widthAnchor constraintEqualToConstant:180.0].active = YES;
    [back.widthAnchor constraintEqualToConstant:180.0].active = YES;

    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[save, reset, back]];
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.alignment = UIStackViewAlignmentCenter;
    buttons.distribution = UIStackViewDistributionEqualSpacing;
    buttons.spacing = 12.0;

    self.settingsStatus = MakeLabel(@"", 13.0, UIFontWeightRegular);
    self.settingsStatus.textAlignment = NSTextAlignmentLeft;
    self.settingsStatus.textColor = [UIColor colorWithWhite:0.65 alpha:1.0];

    [self.settingsView addSubview:title];
    [self.settingsView addSubview:note];
    [self.settingsView addSubview:scroll];
    [self.settingsView addSubview:buttons];
    [self.settingsView addSubview:self.settingsStatus];

    [NSLayoutConstraint activateConstraints:@[
        [title.leadingAnchor constraintEqualToAnchor:self.settingsView.leadingAnchor],
        [title.trailingAnchor constraintEqualToAnchor:self.settingsView.trailingAnchor],
        [title.topAnchor constraintEqualToAnchor:self.settingsView.topAnchor],

        [note.leadingAnchor constraintEqualToAnchor:self.settingsView.leadingAnchor],
        [note.trailingAnchor constraintEqualToAnchor:self.settingsView.trailingAnchor],
        [note.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:4.0],

        [scroll.leadingAnchor constraintEqualToAnchor:self.settingsView.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.settingsView.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:note.bottomAnchor constant:12.0],
        [scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-12.0],

        [controls.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [controls.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [controls.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [controls.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [controls.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],

        [buttons.centerXAnchor constraintEqualToAnchor:self.settingsView.centerXAnchor],
        [buttons.bottomAnchor constraintEqualToAnchor:self.settingsStatus.topAnchor constant:-7.0],

        [self.settingsStatus.leadingAnchor constraintEqualToAnchor:self.settingsView.leadingAnchor],
        [self.settingsStatus.trailingAnchor constraintEqualToAnchor:self.settingsView.trailingAnchor],
        [self.settingsStatus.bottomAnchor constraintEqualToAnchor:self.settingsView.bottomAnchor],
    ]];

    [self resetНастройкиControls];
}

- (NSString *)diagnosticsTextWithGameFileSize:(NSString *)gameFileSize
{
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *shortVersion = [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"unknown";
    NSString *buildVersion = [bundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unknown";

    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    BOOL documentsExists = [[NSFileManager defaultManager] fileExistsAtPath:documents];
    NSString *gameFileStatus = nil;
    BOOL gameFileReady = [[GXGameFileManager sharedManager] validateInstalledGameFile:&gameFileStatus];
    if (gameFileStatus == nil)
        gameFileStatus = gameFileReady ? @"Файл игры: ГОТОВ" : @"Файл игры: НЕ ГОТОВ";
    BOOL enhancedУстановлено = ProfileDirectoryExists(@"enhanced");
    BOOL zeroHourУстановлено = ProfileDirectoryExists(@"zerohour");

    NSString *settingsPath = IOSIPadOverridesPath();
    NSString *zeroHourНастройкиPath = ZeroHourSettingsPath();
    BOOL settingsExists = [[NSFileManager defaultManager] fileExistsAtPath:settingsPath];
    BOOL zeroHourНастройкиExists = [[NSFileManager defaultManager] fileExistsAtPath:zeroHourНастройкиPath];

    NSString *currentLog = DocumentsFilePath(@"generals-stderr.log");
    BOOL currentLogExists = [[NSFileManager defaultManager] fileExistsAtPath:currentLog];
    NSString *currentLogText = currentLogExists
        ? [NSString stringWithFormat:@"Yes (%@)", HumanReadableBytes(FileSizeAtPath(currentLog))]
        : @"No";

    NSUInteger sessionLogCount = 0;
    unsigned long long sessionLogBytes = 0;
    for (NSString *name in DiagnosticSessionLogNames())
    {
        NSString *path = DocumentsFilePath(name);
        if ([[NSFileManager defaultManager] fileExistsAtPath:path])
        {
            ++sessionLogCount;
            sessionLogBytes += FileSizeAtPath(path);
        }
    }

    NSString *sessionLogsText =
        [NSString stringWithFormat:@"%lu/10 (%@)",
                                   (unsigned long)sessionLogCount,
                                   HumanReadableBytes(sessionLogBytes)];

    return [NSString stringWithFormat:
        @"ПРИЛОЖЕНИЕ\n"
         "Проект: %s\n"
         "Пакет: %@ (%@)\n"
         "iOS: %@\n"
         "Устройство: %@\n\n"
         "СБОРКА\n"
         "Лаунчер: v%s · %@\n"
         "Запуск лаунчера: %@\n"
         "Движок: v%s · %@\n"
         "Запуск базовой оболочки: %@\n\n"
         "ФАЙЛ ИГРЫ\n"
         "Documents / файлы игры: %@\n"
         "Размер файла игры: %@\n"
         "%@\n"
         "Enhanced: %@\n"
         "ZeroHour: %@\n\n"
         "ФАЙЛЫ\n"
         "Настройки iOS/iPad: %@\n"
         "Настройки ZeroHour: %@\n"
         "Текущий сеанс: %@\n"
         "Логи сеансов: %@\n",
        GX_PROJECT_VERSION,
        shortVersion,
        buildVersion,
        UIDevice.currentDevice.systemVersion,
        UIDevice.currentDevice.model,
        GX_LAUNCHER_VERSION,
        ShortBuildIdentifier(GX_LAUNCHER_COMMIT),
        ShortBuildIdentifier(GX_LAUNCHER_RUN),
        GX_ENGINE_VERSION,
        ShortBuildIdentifier(GX_ENGINE_COMMIT),
        ShortBuildIdentifier(GX_BASE_SHELL_RUN),
        documentsExists ? (gameFileReady ? @"ГОТОВЫ" : @"НЕ ГОТОВЫ") : @"Documents недоступен",
        gameFileSize,
        gameFileStatus,
        enhancedУстановлено ? @"Установлено" : @"Не установлено",
        zeroHourУстановлено ? @"Установлено" : @"Не установлено",
        settingsExists ? @"Есть" : @"Отсутствует",
        zeroHourНастройкиExists ? @"Есть" : @"Отсутствует",
        currentLogText,
        sessionLogsText];
}

- (void)refreshGameFileStatusCard
{
    if (self.gameFileStatusButton == nil)
        return;

    NSString *message = nil;
    BOOL ready = [[GXGameFileManager sharedManager] validateInstalledGameFile:&message];

    // Сам статус ничего не скачивает. Установка запускается только нижней
    // кнопкой "Файл игры".
    NSString *subtitle = nil;
    if (ready) {
        subtitle = message.length > 0 ? message : @"Файл игры: ГОТОВ";
        if ([subtitle hasPrefix:@"Файл игры: "])
            subtitle = [subtitle substringFromIndex:@"Файл игры: ".length];
    } else {
        subtitle = message.length > 0 ? message : @"Файл игры: НЕ ГОТОВ";
        if (![subtitle containsString:@"скачайте через кнопку"])
            subtitle = [NSString stringWithFormat:@"%@  •  скачайте через кнопку «Файл игры»", subtitle];
    }

    UIColor *accent = ready
        ? [UIColor colorWithRed:0.18 green:0.88 blue:0.48 alpha:1.0]
        : [UIColor colorWithRed:1.0 green:0.55 blue:0.25 alpha:1.0];

    NSString *display = [NSString stringWithFormat:@"%@  СТАТУС ФАЙЛА ИГРЫ\n%@", ready ? @"✓" : @"⚠", subtitle];
    NSMutableAttributedString *attributed = [[NSMutableAttributedString alloc] initWithString:display];
    NSString *titleText = ready ? @"✓  СТАТУС ФАЙЛА ИГРЫ" : @"⚠  СТАТУС ФАЙЛА ИГРЫ";
    [attributed addAttribute:NSFontAttributeName
                       value:[UIFont systemFontOfSize:15.0 weight:UIFontWeightBold]
                       range:NSMakeRange(0, titleText.length)];
    [attributed addAttribute:NSForegroundColorAttributeName
                       value:UIColor.whiteColor
                       range:NSMakeRange(0, titleText.length)];
    [attributed addAttribute:NSFontAttributeName
                       value:[UIFont systemFontOfSize:11.0 weight:UIFontWeightRegular]
                       range:NSMakeRange(titleText.length + 1, subtitle.length)];
    [attributed addAttribute:NSForegroundColorAttributeName
                       value:accent
                       range:NSMakeRange(titleText.length + 1, subtitle.length)];
    [self.gameFileStatusButton setAttributedTitle:attributed forState:UIControlStateNormal];
}

- (void)buildDiagnostics
{
    self.diagnosticsView = [[UIView alloc] init];
    self.diagnosticsView.translatesAutoresizingMaskIntoConstraints = NO;
    self.diagnosticsView.backgroundColor = [UIColor colorWithWhite:0.02 alpha:0.56];
    self.diagnosticsView.layer.cornerRadius = 28.0;
    self.diagnosticsView.layer.borderWidth = 1.0;
    self.diagnosticsView.layer.borderColor = [UIColor colorWithRed:0.25 green:0.60 blue:1.0 alpha:0.45].CGColor;
    self.diagnosticsView.layer.shadowColor = UIColor.blackColor.CGColor;
    self.diagnosticsView.layer.shadowOpacity = 0.45;
    self.diagnosticsView.layer.shadowRadius = 28.0;
    self.diagnosticsView.layer.shadowOffset = CGSizeMake(0, 12);
    MakeGlassBlurView(self.diagnosticsView);
    self.diagnosticsView.hidden = YES;
    [self.view addSubview:self.diagnosticsView];

    [NSLayoutConstraint activateConstraints:@[
        [self.diagnosticsView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:28.0],
        [self.diagnosticsView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-28.0],
        [self.diagnosticsView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:18.0],
        [self.diagnosticsView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-18.0],
    ]];

    UILabel *title = MakeLabel(@"Диагностика", 26.0, UIFontWeightBold);
    title.textAlignment = NSTextAlignmentLeft;

    UILabel *note = MakeLabel(@"Сборка, установленный контент и логи сбоев. Последние 10 сеансов приложения сохраняются автоматически.", 13.0, UIFontWeightRegular);
    note.textAlignment = NSTextAlignmentLeft;
    note.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];

    UIScrollView *scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = YES;
    scroll.showsVerticalScrollIndicator = YES;

    self.diagnosticsText = MakeLabel(@"", 15.0, UIFontWeightRegular);
    self.diagnosticsText.textAlignment = NSTextAlignmentLeft;
    self.diagnosticsText.font = [UIFont monospacedSystemFontOfSize:15.0 weight:UIFontWeightRegular];
    [scroll addSubview:self.diagnosticsText];

    UIButton *refresh = MakeButton(@"Обновить", self, @selector(refreshDiagnostics));
    self.shareDiagnosticsButton = MakeButton(@"Поделиться отчётом + логами", self, @selector(shareDiagnostics));
    UIButton *clearLogs = MakeButton(@"Очистить логи", self, @selector(clearDiagnosticsLogs));
    UIButton *back = MakeButton(@"Назад", self, @selector(hideDiagnostics));

    clearLogs.backgroundColor = [UIColor colorWithRed:0.24 green:0.06 blue:0.06 alpha:1.0];

    [refresh.widthAnchor constraintEqualToConstant:160.0].active = YES;
    [self.shareDiagnosticsButton.widthAnchor constraintEqualToConstant:220.0].active = YES;
    [clearLogs.widthAnchor constraintEqualToConstant:160.0].active = YES;
    [back.widthAnchor constraintEqualToConstant:160.0].active = YES;

    UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[
        refresh, self.shareDiagnosticsButton, clearLogs, back
    ]];
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.alignment = UIStackViewAlignmentCenter;
    buttons.spacing = 12.0;

    [self.diagnosticsView addSubview:title];
    [self.diagnosticsView addSubview:note];
    [self.diagnosticsView addSubview:scroll];
    [self.diagnosticsView addSubview:buttons];

    [NSLayoutConstraint activateConstraints:@[
        [title.leadingAnchor constraintEqualToAnchor:self.diagnosticsView.leadingAnchor],
        [title.trailingAnchor constraintEqualToAnchor:self.diagnosticsView.trailingAnchor],
        [title.topAnchor constraintEqualToAnchor:self.diagnosticsView.topAnchor],

        [note.leadingAnchor constraintEqualToAnchor:self.diagnosticsView.leadingAnchor],
        [note.trailingAnchor constraintEqualToAnchor:self.diagnosticsView.trailingAnchor],
        [note.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:4.0],

        [scroll.leadingAnchor constraintEqualToAnchor:self.diagnosticsView.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.diagnosticsView.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:note.bottomAnchor constant:14.0],
        [scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-14.0],

        [self.diagnosticsText.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [self.diagnosticsText.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [self.diagnosticsText.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [self.diagnosticsText.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [self.diagnosticsText.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],

        [buttons.centerXAnchor constraintEqualToAnchor:self.diagnosticsView.centerXAnchor],
        [buttons.bottomAnchor constraintEqualToAnchor:self.diagnosticsView.bottomAnchor],
    ]];
}

- (void)buildProfileModal
{
    self.profileView = [[UIView alloc] init];
    self.profileView.translatesAutoresizingMaskIntoConstraints = NO;
    self.profileView.backgroundColor = [UIColor colorWithWhite:0.025 alpha:0.72];
    self.profileView.layer.cornerRadius = 26.0;
    self.profileView.layer.borderWidth = 1.0;
    self.profileView.layer.borderColor = [UIColor colorWithRed:0.25 green:0.60 blue:1.0 alpha:0.48].CGColor;
    self.profileView.layer.shadowColor = UIColor.blackColor.CGColor;
    self.profileView.layer.shadowOpacity = 0.55;
    self.profileView.layer.shadowRadius = 30.0;
    self.profileView.layer.shadowOffset = CGSizeMake(0, 14);
    self.profileView.hidden = YES;
    [self.view addSubview:self.profileView];
    MakeGlassBlurView(self.profileView);

    UILabel *title = MakeLabel(@"ПРОФИЛЬ", 27.0, UIFontWeightBold);
    title.textAlignment = NSTextAlignmentLeft;
    [self.profileView addSubview:title];

    UILabel *note = MakeLabel(@"Выберите профиль запуска игры", 14.0, UIFontWeightRegular);
    note.textAlignment = NSTextAlignmentLeft;
    note.textColor = [UIColor colorWithWhite:0.65 alpha:1.0];
    [self.profileView addSubview:note];

    UIButton *vanilla = MakeButton(@"VANILLA\nОбычный запуск", self, @selector(launchVanilla));
    UIButton *enhanced = MakeButton(@"ENHANCED\nРасширенный профиль", self, @selector(launchEnhanced));
    UIButton *zeroHour = MakeButton(@"ZERO HOUR\nОсновной профиль", self, @selector(launchZeroHour));
    UIButton *close = MakeButton(@"Закрыть", self, @selector(hideProfile));

    vanilla.backgroundColor = [UIColor colorWithRed:0.04 green:0.25 blue:0.50 alpha:0.72];
    enhanced.backgroundColor = [UIColor colorWithRed:0.12 green:0.16 blue:0.30 alpha:0.72];
    zeroHour.backgroundColor = [UIColor colorWithRed:0.30 green:0.12 blue:0.06 alpha:0.72];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        vanilla, enhanced, zeroHour, close
    ]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 11.0;
    [self.profileView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [self.profileView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:90.0],
        [self.profileView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-90.0],
        [self.profileView.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [self.profileView.heightAnchor constraintGreaterThanOrEqualToConstant:390.0],

        [title.leadingAnchor constraintEqualToAnchor:self.profileView.leadingAnchor constant:24.0],
        [title.trailingAnchor constraintEqualToAnchor:self.profileView.trailingAnchor constant:-24.0],
        [title.topAnchor constraintEqualToAnchor:self.profileView.topAnchor constant:24.0],

        [note.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [note.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [note.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:5.0],

        [stack.leadingAnchor constraintEqualToAnchor:self.profileView.leadingAnchor constant:24.0],
        [stack.trailingAnchor constraintEqualToAnchor:self.profileView.trailingAnchor constant:-24.0],
        [stack.topAnchor constraintEqualToAnchor:note.bottomAnchor constant:18.0],
        [stack.bottomAnchor constraintEqualToAnchor:self.profileView.bottomAnchor constant:-24.0]
    ]];
}

- (void)showProfile
{
    self.menuStack.hidden = NO;
    self.settingsView.hidden = YES;
    self.diagnosticsView.hidden = YES;
    self.modalBackdrop.hidden = NO;
    self.profileView.hidden = NO;
}

- (void)hideProfile
{
    self.profileView.hidden = YES;
    self.modalBackdrop.hidden = YES;
    self.menuStack.hidden = NO;
}

- (void)showDiagnostics
{
    self.menuStack.hidden = NO;
    self.settingsView.hidden = YES;
    self.profileView.hidden = YES;
    self.modalBackdrop.hidden = NO;
    self.diagnosticsView.hidden = NO;
    [self refreshDiagnostics];
    [self refreshGameFileStatusCard];
}

- (void)hideDiagnostics
{
    self.diagnosticsView.hidden = YES;
    self.modalBackdrop.hidden = YES;
    self.menuStack.hidden = NO;
}

- (void)refreshDiagnostics
{
    if (self.diagnosticsScanRunning)
        return;

    self.diagnosticsScanRunning = YES;
    self.diagnosticsText.text = [self diagnosticsTextWithGameFileSize:@"Вычисление…"];

    NSString *gameRoot = GameRootPath();
    BOOL exists = [[NSFileManager defaultManager] fileExistsAtPath:gameRoot];

    __weak GXProfileLauncherViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        unsigned long long bytes = exists ? InstalledGameFilesSizeAtDocuments() : 0;
        NSString *sizeText = exists ? HumanReadableBytes(bytes) : @"нет данных";

        dispatch_async(dispatch_get_main_queue(), ^{
            GXProfileLauncherViewController *strongSelf = weakSelf;
            if (strongSelf == nil)
                return;

            strongSelf.diagnosticsScanRunning = NO;
            strongSelf.diagnosticsText.text =
                [strongSelf diagnosticsTextWithGameFileSize:sizeText];
        });
    });
}

- (void)clearDiagnosticsLogs
{
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Очистить диагностические логи?"
                                            message:@"Это удалит лог текущего сеанса и все сохранённые логи сеансов."
                                     preferredStyle:UIAlertControllerStyleAlert];

    [alert addAction:[UIAlertAction actionWithTitle:@"Отмена"
                                             style:UIAlertActionStyleCancel
                                           handler:nil]];

    __weak GXProfileLauncherViewController *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Очистить логи"
                                             style:UIAlertActionStyleDestructive
                                           handler:^(__unused UIAlertAction *action) {
        if (gDiagnosticClearCallback != nullptr)
        {
            // Full shell: ask the engine to reset its live logger FD and size/cap bookkeeping.
            gDiagnosticClearCallback();
        }
        else
        {
            // Launcher-only fast builds can be overlaid on an older successful shell.
            // In that case no callback is installed, so clear only the visible files.
            NSFileManager *fileManager = [NSFileManager defaultManager];
            for (NSString *name in DiagnosticSessionLogNames())
                [fileManager removeItemAtPath:DocumentsFilePath(name) error:nil];
            [fileManager removeItemAtPath:DocumentsFilePath(@"generals-stderr-prev.log") error:nil];
        }

        GXProfileLauncherViewController *strongSelf = weakSelf;
        if (strongSelf != nil)
        {
            strongSelf.diagnosticsScanRunning = NO;
            [strongSelf refreshDiagnostics];
        }
    }]];

    [self presentViewController:alert animated:YES completion:nil];
}


- (void)shareDiagnostics
{
    NSMutableArray *items = [NSMutableArray array];

    NSString *reportPath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"GeneralsZH-Диагностика.txt"];
    NSError *writeError = nil;
    BOOL wroteReport = [self.diagnosticsText.text writeToFile:reportPath
                                                  atomically:YES
                                                    encoding:NSUTF8StringEncoding
                                                       error:&writeError];
    if (wroteReport)
        [items addObject:[NSURL fileURLWithPath:reportPath]];
    else
        [items addObject:self.diagnosticsText.text ?: @"Диагностика Generals ZH недоступна"];

    for (NSString *name in DiagnosticSessionLogNames())
    {
        NSString *path = DocumentsFilePath(name);
        if ([[NSFileManager defaultManager] fileExistsAtPath:path])
            [items addObject:[NSURL fileURLWithPath:path]];
    }

    UIActivityViewController *activity =
        [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];

    UIPopoverPresentationController *popover = activity.popoverPresentationController;
    if (popover != nil)
    {
        popover.sourceView = self.shareDiagnosticsButton;
        popover.sourceRect = self.shareDiagnosticsButton.bounds;
    }

    [self presentViewController:activity animated:YES completion:nil];

    if (!wroteReport && writeError != nil)
    {
        fprintf(stderr, "WARNING: failed to write diagnostics report: %s\n",
                [[writeError description] UTF8String]);
    }
}

- (BOOL)prefersStatusBarHidden
{
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)shouldAutorotate
{
    return YES;
}

- (void)launchVanilla
{
    SetSelectedProfile(@"vanilla");
}

- (void)launchEnhanced
{
    SetSelectedProfile(@"enhanced");
}

- (void)launchZeroHour
{
    SetSelectedProfile(@"zerohour");
}


- (void)startGameFileBackgroundVideo
{
    if (self.gameFileView == nil || self.gameFileVideoPlayer != nil)
        return;

    NSURL *videoURL = [[NSBundle mainBundle] URLForResource:@"intro" withExtension:@"mp4"];
    if (videoURL == nil)
    {
        fprintf(stderr, "WARNING: iOS launcher install video resources/intro.mp4 not found in app bundle.\n");
        return;
    }

    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:videoURL];
    self.gameFileVideoPlayer = [AVPlayer playerWithPlayerItem:item];
    self.gameFileVideoPlayer.volume = 0.30;
    self.gameFileVideoPlayer.actionAtItemEnd = AVPlayerActionAtItemEndNone;

    self.gameFileVideoLayer = [AVPlayerLayer playerLayerWithPlayer:self.gameFileVideoPlayer];
    self.gameFileVideoLayer.videoGravity = AVLayerVideoGravityResizeAspectFill;
    self.gameFileVideoLayer.opacity = 1.0;
    // The video is the first sublayer of the transparent installer view.\n    // Do not use a negative zPosition: that can place AVPlayerLayer behind\n    // the host view on some iOS Core Animation compositing paths.\n    self.gameFileVideoLayer.frame = self.gameFileView.bounds;
    [self.gameFileView.layer insertSublayer:self.gameFileVideoLayer atIndex:0];

    __weak GXProfileLauncherViewController *weakSelf = self;
    self.gameFileVideoLoopObserver =
        [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                                          object:item
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification *notification) {
        GXProfileLauncherViewController *strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf.gameFileVideoPlayer == nil)
            return;

        [strongSelf.gameFileVideoPlayer seekToTime:kCMTimeZero
                                  toleranceBefore:kCMTimeZero
                                   toleranceAfter:kCMTimeZero
                                completionHandler:^(BOOL finished) {
            if (finished && strongSelf.gameFileVideoPlayer != nil)
                [strongSelf.gameFileVideoPlayer play];
        }];
    }];

    // Edge blur only: four soft/fading blur zones hug the screen edges.
    // There is deliberately no square/rectangular blur panel over the content.
    self.gameFileEdgeBlurViews = [NSMutableArray array];
    NSArray<NSNumber *> *alphas = @[@0.13, @0.13, @0.11, @0.11];
    for (NSInteger index = 0; index < 4; ++index)
    {
        UIVisualEffectView *edge = [[UIVisualEffectView alloc]
            initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
        edge.translatesAutoresizingMaskIntoConstraints = NO;
        edge.userInteractionEnabled = NO;
        edge.alpha = alphas[index].doubleValue;
        [self.gameFileView addSubview:edge];
        [self.gameFileEdgeBlurViews addObject:edge];

        CAGradientLayer *mask = [CAGradientLayer layer];
        mask.startPoint = CGPointMake(0.0, 0.0);
        mask.endPoint = CGPointMake(1.0, 0.0);
        mask.colors = @[
            (id)UIColor.whiteColor.CGColor,
            (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor
        ];
        edge.layer.mask = mask;
    }

    UIVisualEffectView *top = self.gameFileEdgeBlurViews[0];
    UIVisualEffectView *bottom = self.gameFileEdgeBlurViews[1];
    UIVisualEffectView *left = self.gameFileEdgeBlurViews[2];
    UIVisualEffectView *right = self.gameFileEdgeBlurViews[3];

    [NSLayoutConstraint activateConstraints:@[
        [top.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor],
        [top.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor],
        [top.topAnchor constraintEqualToAnchor:self.gameFileView.topAnchor],
        [top.heightAnchor constraintEqualToAnchor:self.gameFileView.heightAnchor multiplier:0.18],

        [bottom.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor],
        [bottom.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor],
        [bottom.bottomAnchor constraintEqualToAnchor:self.gameFileView.bottomAnchor],
        [bottom.heightAnchor constraintEqualToAnchor:self.gameFileView.heightAnchor multiplier:0.18],

        [left.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor],
        [left.topAnchor constraintEqualToAnchor:self.gameFileView.topAnchor],
        [left.bottomAnchor constraintEqualToAnchor:self.gameFileView.bottomAnchor],
        [left.widthAnchor constraintEqualToAnchor:self.gameFileView.widthAnchor multiplier:0.13],

        [right.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor],
        [right.topAnchor constraintEqualToAnchor:self.gameFileView.topAnchor],
        [right.bottomAnchor constraintEqualToAnchor:self.gameFileView.bottomAnchor],
        [right.widthAnchor constraintEqualToAnchor:self.gameFileView.widthAnchor multiplier:0.13]
    ]];

    // Correct the horizontal gradient directions for bottom/right/vertical edges.
    CAGradientLayer *bottomMask = [CAGradientLayer layer];
    bottomMask.startPoint = CGPointMake(0.0, 1.0);
    bottomMask.endPoint = CGPointMake(0.0, 0.0);
    bottomMask.colors = @[
        (id)UIColor.whiteColor.CGColor,
        (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor
    ];
    bottom.layer.mask = bottomMask;

    CAGradientLayer *leftMask = [CAGradientLayer layer];
    leftMask.startPoint = CGPointMake(0.0, 0.0);
    leftMask.endPoint = CGPointMake(1.0, 0.0);
    leftMask.colors = @[
        (id)UIColor.whiteColor.CGColor,
        (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor
    ];
    left.layer.mask = leftMask;

    CAGradientLayer *rightMask = [CAGradientLayer layer];
    rightMask.startPoint = CGPointMake(1.0, 0.0);
    rightMask.endPoint = CGPointMake(0.0, 0.0);
    rightMask.colors = @[
        (id)UIColor.whiteColor.CGColor,
        (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor
    ];
    right.layer.mask = rightMask;

    [self.gameFileView bringSubviewToFront:self.gameFileStage];
    [self.gameFileView bringSubviewToFront:self.gameFilePercentLabel];
    [self.gameFileView bringSubviewToFront:self.gameFileProgress];
    [self.gameFileView bringSubviewToFront:self.gameFileDetail];
    [self.gameFileView bringSubviewToFront:self.gameFileCancelButton];
    [self.gameFileView bringSubviewToFront:self.gameFileReinstallButton];
    [self.gameFileView bringSubviewToFront:self.gameFileMinimizeButton];
    [self.gameFileView bringSubviewToFront:self.gameFileCloseButton];

    [self.gameFileVideoPlayer play];
    fprintf(stderr, "INFO: iOS launcher install video started: intro.mp4, volume=30%%, edge blur=10-13%%.\n");
}

- (void)stopGameFileBackgroundVideo
{
    if (self.gameFileVideoLoopObserver != nil)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:self.gameFileVideoLoopObserver];
        self.gameFileVideoLoopObserver = nil;
    }

    [self.gameFileVideoPlayer pause];
    self.gameFileVideoPlayer = nil;

    [self.gameFileVideoLayer removeFromSuperlayer];
    self.gameFileVideoLayer = nil;

    for (UIVisualEffectView *edge in self.gameFileEdgeBlurViews)
        [edge removeFromSuperview];
    [self.gameFileEdgeBlurViews removeAllObjects];
    self.gameFileEdgeBlurViews = nil;
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    if (self.gameFileVideoLayer != nil) {
        self.gameFileVideoLayer.frame = self.gameFileView.bounds;
        // Keep the player as the first sublayer; UIKit controls render above it.
    }
}

- (void)dealloc
{
    [self stopGameFileBackgroundVideo];
}

- (void)showGameFileProgress
{
    self.menuStack.hidden = YES;
    self.settingsView.hidden = YES;
    self.diagnosticsView.hidden = YES;
    self.profileView.hidden = YES;
    self.modalBackdrop.hidden = YES;

    [self.gameFileView removeFromSuperview];

    // GameFile — отдельное полноэкранное окно, а не часть modalBackdrop.
    self.gameFileView = [[UIView alloc] init];
    self.gameFileView.translatesAutoresizingMaskIntoConstraints = NO;
    // Transparent overlay: the intro video must remain visible behind the loading UI.
    self.gameFileView.backgroundColor = UIColor.clearColor;
    [self.view addSubview:self.gameFileView];

    [NSLayoutConstraint activateConstraints:@[
        [self.gameFileView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.gameFileView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.gameFileView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.gameFileView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];

    UIView *topBar = [[UIView alloc] init];
    topBar.translatesAutoresizingMaskIntoConstraints = NO;
    topBar.backgroundColor = [UIColor colorWithWhite:0.02 alpha:0.48];
    topBar.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.08].CGColor;
    topBar.layer.borderWidth = 1.0;
    [self.gameFileView addSubview:topBar];

    UILabel *title = MakeLabel(@"ФАЙЛ ИГРЫ", 26.0, UIFontWeightBold);
    title.textAlignment = NSTextAlignmentLeft;
    [topBar addSubview:title];

    UILabel *subtitle = MakeLabel(@"Установка файлов игры", 13.0, UIFontWeightRegular);
    subtitle.textAlignment = NSTextAlignmentLeft;
    subtitle.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
    [topBar addSubview:subtitle];

    self.gameFileMinimizeButton = MakeButton(@"⌄", self, @selector(minimizeGameFileProgress));
    self.gameFileCloseButton = MakeButton(@"×", self, @selector(closeGameFileProgress));
    self.gameFileMinimizeButton.titleLabel.font = [UIFont systemFontOfSize:24.0 weight:UIFontWeightSemibold];
    self.gameFileMinimizeButton.accessibilityLabel = @"Свернуть установку";
    self.gameFileCloseButton.titleLabel.font = [UIFont systemFontOfSize:22.0 weight:UIFontWeightRegular];
    [self.gameFileMinimizeButton.widthAnchor constraintEqualToConstant:52.0].active = YES;
    [self.gameFileCloseButton.widthAnchor constraintEqualToConstant:52.0].active = YES;
    [topBar addSubview:self.gameFileMinimizeButton];
    [topBar addSubview:self.gameFileCloseButton];

    self.gameFileStage = MakeLabel(@"Скачивание", 20.0, UIFontWeightSemibold);
    self.gameFileStage.layer.shadowColor = UIColor.blackColor.CGColor;
    self.gameFileStage.layer.shadowOpacity = 0.9;
    self.gameFileStage.layer.shadowRadius = 5.0;
    self.gameFileStage.textAlignment = NSTextAlignmentLeft;
    [self.gameFileView addSubview:self.gameFileStage];

    self.gameFilePercentLabel = MakeLabel(@"0%", 18.0, UIFontWeightBold);
    self.gameFilePercentLabel.textAlignment = NSTextAlignmentRight;
    self.gameFilePercentLabel.textColor = [UIColor colorWithRed:0.38 green:0.72 blue:1.0 alpha:1.0];
    [self.gameFileView addSubview:self.gameFilePercentLabel];

    self.gameFileProgress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.gameFileProgress.translatesAutoresizingMaskIntoConstraints = NO;
    self.gameFileProgress.progress = 0.0;
    self.gameFileProgress.trackTintColor = [UIColor colorWithWhite:1.0 alpha:0.10];
    self.gameFileProgress.progressTintColor = [UIColor colorWithRed:0.25 green:0.65 blue:1.0 alpha:1.0];
    [self.gameFileView addSubview:self.gameFileProgress];

    self.gameFileDetail = MakeLabel(@"Подключение…", 15.0, UIFontWeightRegular);
    self.gameFileDetail.layer.shadowColor = UIColor.blackColor.CGColor;
    self.gameFileDetail.layer.shadowOpacity = 0.9;
    self.gameFileDetail.layer.shadowRadius = 4.0;
    self.gameFileDetail.textAlignment = NSTextAlignmentLeft;
    self.gameFileDetail.textColor = [UIColor colorWithWhite:0.76 alpha:1.0];
    self.gameFileDetail.numberOfLines = 0;
    [self.gameFileView addSubview:self.gameFileDetail];

    self.gameFileCancelButton = MakeButton(@"Отменить загрузку", self, @selector(cancelGameFileDownload));
    self.gameFileCancelButton.backgroundColor = [UIColor colorWithRed:0.40 green:0.08 blue:0.08 alpha:0.78];
    [self.gameFileView addSubview:self.gameFileCancelButton];

    self.gameFileReinstallButton = MakeButton(@"Переустановить игру", self, @selector(reinstallGameFile));
    self.gameFileReinstallButton.backgroundColor = [UIColor colorWithRed:0.08 green:0.30 blue:0.48 alpha:0.86];
    [self.gameFileView addSubview:self.gameFileReinstallButton];

    [NSLayoutConstraint activateConstraints:@[
        [topBar.topAnchor constraintEqualToAnchor:self.gameFileView.topAnchor],
        [topBar.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor],
        [topBar.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor],
        [topBar.heightAnchor constraintEqualToConstant:92.0],

        [title.leadingAnchor constraintEqualToAnchor:topBar.leadingAnchor constant:28.0],
        [title.topAnchor constraintEqualToAnchor:topBar.topAnchor constant:18.0],
        [subtitle.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [subtitle.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:2.0],

        [self.gameFileCloseButton.trailingAnchor constraintEqualToAnchor:topBar.trailingAnchor constant:-18.0],
        [self.gameFileCloseButton.centerYAnchor constraintEqualToAnchor:topBar.centerYAnchor],
        [self.gameFileMinimizeButton.trailingAnchor constraintEqualToAnchor:self.gameFileCloseButton.leadingAnchor constant:-8.0],
        [self.gameFileMinimizeButton.centerYAnchor constraintEqualToAnchor:topBar.centerYAnchor],

        [self.gameFileStage.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor constant:48.0],
        [self.gameFileStage.topAnchor constraintEqualToAnchor:topBar.bottomAnchor constant:54.0],
        [self.gameFileStage.trailingAnchor constraintEqualToAnchor:self.gameFilePercentLabel.leadingAnchor constant:-18.0],

        [self.gameFilePercentLabel.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor constant:-48.0],
        [self.gameFilePercentLabel.centerYAnchor constraintEqualToAnchor:self.gameFileStage.centerYAnchor],

        [self.gameFileProgress.leadingAnchor constraintEqualToAnchor:self.gameFileView.leadingAnchor constant:48.0],
        [self.gameFileProgress.trailingAnchor constraintEqualToAnchor:self.gameFileView.trailingAnchor constant:-48.0],
        [self.gameFileProgress.topAnchor constraintEqualToAnchor:self.gameFileStage.bottomAnchor constant:18.0],

        [self.gameFileDetail.leadingAnchor constraintEqualToAnchor:self.gameFileProgress.leadingAnchor],
        [self.gameFileDetail.trailingAnchor constraintEqualToAnchor:self.gameFileProgress.trailingAnchor],
        [self.gameFileDetail.topAnchor constraintEqualToAnchor:self.gameFileProgress.bottomAnchor constant:22.0],

        [self.gameFileCancelButton.leadingAnchor constraintEqualToAnchor:self.gameFileProgress.leadingAnchor],
        [self.gameFileCancelButton.bottomAnchor constraintEqualToAnchor:self.gameFileView.safeAreaLayoutGuide.bottomAnchor constant:-28.0],
        [self.gameFileCancelButton.widthAnchor constraintEqualToConstant:230.0],

        [self.gameFileReinstallButton.leadingAnchor constraintEqualToAnchor:self.gameFileProgress.leadingAnchor],
        [self.gameFileReinstallButton.bottomAnchor constraintEqualToAnchor:self.gameFileView.safeAreaLayoutGuide.bottomAnchor constant:-28.0],
        [self.gameFileReinstallButton.widthAnchor constraintEqualToConstant:230.0]
    ]];

    NSString *validationMessage = nil;
    BOOL installed = [[GXGameFileManager sharedManager] validateInstalledGameFile:&validationMessage];
    self.gameFileCancelButton.hidden = installed;
    self.gameFileReinstallButton.hidden = !installed;
    if (installed) {
        self.gameFileStage.text = @"Игра уже установлена";
        self.gameFileDetail.text = validationMessage.length > 0
            ? validationMessage
            : @"Файлы игры найдены и готовы к запуску. Если нужно, нажмите «Переустановить игру».";
    }

}

- (void)minimizeGameFileProgress
{
    self.gameFileView.hidden = YES;
    self.menuStack.hidden = NO;
}

- (void)closeGameFileProgress
{
    self.gameFileView.hidden = YES;
    self.menuStack.hidden = NO;
}

- (void)cancelGameFileDownload
{
    [[GXGameFileManager sharedManager] cancelDownload];
    [self stopGameFileBackgroundVideo];
    self.gameFileStage.text = @"Отмена…";
    self.gameFileDetail.text = @"Отмена загрузки и распаковки файла игры…";
    self.gameFileCancelButton.enabled = NO;
    self.gameFileReinstallButton.enabled = NO;
}

- (void)hideGameFileProgress
{
    [self stopGameFileBackgroundVideo];
    [self.gameFileView removeFromSuperview];
    self.gameFileView = nil;
    self.gameFileProgress = nil;
    self.gameFileStage = nil;
    self.gameFileDetail = nil;
    self.gameFileCancelButton = nil;
    self.gameFileReinstallButton = nil;
    self.gameFileMinimizeButton = nil;
    self.gameFileCloseButton = nil;
    self.gameFilePercentLabel = nil;
    self.modalBackdrop.hidden = YES;
    self.menuStack.hidden = NO;
}

- (NSString *)gxFormatBytes:(int64_t)bytes
{
    if (bytes < 1024) return [NSString stringWithFormat:@"%lld B", bytes];
    if (bytes < 1024 * 1024) return [NSString stringWithFormat:@"%.1f KB", bytes / 1024.0];
    if (bytes < 1024LL * 1024LL * 1024LL) return [NSString stringWithFormat:@"%.1f MB", bytes / (1024.0 * 1024.0)];
    return [NSString stringWithFormat:@"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0)];
}

- (NSString *)gxFormatTime:(NSTimeInterval)seconds
{
    if (seconds <= 0 || !isfinite(seconds)) return @"—";
    NSInteger s = (NSInteger)ceil(seconds);
    return [NSString stringWithFormat:@"%02ld:%02ld", (long)(s / 60), (long)(s % 60)];
}

- (void)downloadGameFile
{
    // Always open the installer. When the game is already installed, the
    // installer screen shows a dedicated «Переустановить игру» button.
    [self showGameFileProgress];

    if ([[GXGameFileManager sharedManager] validateInstalledGameFile:nil])
        return;

    [self beginGameFileDownload];
}

- (void)reinstallGameFile
{
    if (self.gameFileReinstallButton != nil)
        self.gameFileReinstallButton.hidden = YES;
    [self beginGameFileDownload];
}

- (void)beginGameFileDownload
{
    self.gameFileCancelButton.hidden = NO;
    self.gameFileCancelButton.enabled = YES;
    self.gameFileReinstallButton.hidden = YES;
    self.gameFileProgress.progress = 0.0;
    self.gameFilePercentLabel.text = @"0%";
    self.gameFileStage.text = @"Скачивание";
    self.gameFileDetail.text = @"Подключение…";
    [self stopGameFileBackgroundVideo];
    [self startGameFileBackgroundVideo];

    __weak GXProfileLauncherViewController *weakSelf = self;
    [[GXGameFileManager sharedManager]
        downloadAndInstallGameFileWithProgress:^(double progress, int64_t received, int64_t total, double speed, NSTimeInterval remaining) {
            GXProfileLauncherViewController *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            if (strongSelf.gameFileView.hidden) return;
            strongSelf.gameFileProgress.progress = (float)MAX(0.0, MIN(1.0, progress));
            strongSelf.gameFilePercentLabel.text = [NSString stringWithFormat:@"%.0f%%", progress * 100.0];
            strongSelf.gameFileStage.text = [NSString stringWithFormat:@"Скачивание  •  %.0f%%", progress * 100.0];
            NSString *sizeText = total > 0
                ? [NSString stringWithFormat:@"%@ из %@", [strongSelf gxFormatBytes:received], [strongSelf gxFormatBytes:total]]
                : [strongSelf gxFormatBytes:received];
            NSString *speedText = speed > 0 ? [NSString stringWithFormat:@"Скорость: %@/с", [strongSelf gxFormatBytes:(int64_t)speed]] : @"Скорость: —";
            NSString *timeText = [NSString stringWithFormat:@"Осталось: %@", [strongSelf gxFormatTime:remaining]];
            strongSelf.gameFileDetail.text = [NSString stringWithFormat:@"%@\n%@\n%@", sizeText, speedText, timeText];
        } status:^(NSString *stage, NSString *detail) {
            GXProfileLauncherViewController *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            if (strongSelf.gameFileView.hidden) return;
            strongSelf.gameFileStage.text = stage;
            strongSelf.gameFileDetail.text = detail;
        } completion:^(BOOL success, NSString *message) {
            GXProfileLauncherViewController *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            [strongSelf stopGameFileBackgroundVideo];
            if (strongSelf.gameFileView != nil) {
                strongSelf.gameFileProgress.progress = success ? 1.0 : strongSelf.gameFileProgress.progress;
                strongSelf.gameFilePercentLabel.text = success ? @"100%" : @"";
                strongSelf.gameFileStage.text = success ? @"✓ ФАЙЛ ИГРЫ ГОТОВ" : @"✕ ЗАВЕРШЕНО";
                strongSelf.gameFileDetail.text = message ?: @"";
                if (success) strongSelf.gameFileProgress.progressTintColor = [UIColor colorWithRed:0.18 green:0.88 blue:0.48 alpha:1.0];
                strongSelf.gameFileCancelButton.enabled = NO;
            }
            [strongSelf refreshGameFileStatusCard];
            [strongSelf refreshDiagnostics];

            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [strongSelf hideGameFileProgress];
                [strongSelf loadНастройкиControls];
            });
        }];
}
- (void)saveНастройки
{
    // Save both settings files atomically. Keep all file I/O on the UI action,
    // but never terminate the launcher if a write fails.
    EnsureGameRootDirectory();

    NSString *iosOverrides =
        [NSString stringWithFormat:
            @"GameData\n"
             "  MaxCameraHeight = %.1f\n"
             "  MinCameraHeight = %.1f\n"
             "  CameraPitch = %.1f\n"
             "  EnforceMaxCameraHeight = %@\n"
             "  KeyboardScrollSpeedFactor = %.2f\n"
             "  TerrainDrawDistanceScale = %.2f\n"
             "  UseFPSLimit = %@\n"
             "  FramesPerSecondLimit = %.0f\n"
             "End\n",
            self.maxCameraSlider.value,
            self.minCameraSlider.value,
            self.cameraPitchSlider.value,
            self.enforceMaxSwitch.on ? @"Yes" : @"No",
            self.scrollSpeedSlider.value,
            self.drawDistanceSlider.value,
            self.fpsLimitSwitch.on ? @"Yes" : @"No",
            self.fpsSlider.value];

    NSError *iosError = nil;
    BOOL iosOK = [iosOverrides writeToFile:IOSIPadOverridesPath()
                                   atomically:YES
                                     encoding:NSUTF8StringEncoding
                                        error:&iosError];

    NSString *controlBar = @[@"ZeroHour", @"Pro", @"Standard"][MAX(0, MIN(2, self.zeroHourControlBarSegment.selectedSegmentIndex))];
    NSString *cameos = @[@"Standard", @"HD"][MAX(0, MIN(1, self.zeroHourCameosSegment.selectedSegmentIndex))];
    NSString *music = @[@"Standard", @"Enhanced", @"The Score"][MAX(0, MIN(2, self.zeroHourMusicSegment.selectedSegmentIndex))];
    NSString *voices = @[@"English", @"Native"][MAX(0, MIN(1, self.zeroHourVoicesSegment.selectedSegmentIndex))];
    NSString *hotkeys = @[@"Original", @"Leikeze"][MAX(0, MIN(1, self.zeroHourHotkeysSegment.selectedSegmentIndex))];
    NSString *hotkeyLanguage = @[@"English", @"Russian"][MAX(0, MIN(1, self.zeroHourHotkeyLanguageSegment.selectedSegmentIndex))];
    NSString *portraits = @[@"Standard", @"Funny"][MAX(0, MIN(1, self.zeroHourPortraitsSegment.selectedSegmentIndex))];

    NSInteger textureReduction = MAX(0, MIN(2, self.textureQualitySegment.selectedSegmentIndex));
    NSInteger particleCount = self.particleQualitySegment.selectedSegmentIndex <= 0
        ? 1200
        : (self.particleQualitySegment.selectedSegmentIndex >= 2 ? 4000 : 2500);
    NSString *textureFilter = @[@"Bilinear", @"Trilinear", @"Anisotropic"][MAX(0, MIN(2, self.textureFilterSegment.selectedSegmentIndex))];

    NSDictionary<NSString *, NSString *> *zeroHourValues = @{
        @"AnisotropyLevel": @"8",
        @"BuildingOcclusion": self.buildingOcclusionSwitch.on ? @"Yes" : @"No",
        @"Cameos": cameos,
        @"ControlBar": controlBar,
        @"DynamicLOD": self.dynamicLODSwitch.on ? @"Yes" : @"No",
        @"ExtraAnimations": self.extraAnimationsSwitch.on ? @"Yes" : @"No",
        @"ExtraBuildingProps": self.zeroHourExtraBuildingPropsSwitch.on ? @"Yes" : @"No",
        @"FogEffects": self.zeroHourFogSwitch.on ? @"Yes" : @"No",
        @"HeatEffects": self.heatEffectsSwitch.on ? @"Yes" : @"No",
        @"HotkeyLanguage": hotkeyLanguage,
        @"Hotkeys": hotkeys,
        @"MaxParticleCount": [NSString stringWithFormat:@"%ld", (long)particleCount],
        @"Music": music,
        @"Portraits": portraits,
        @"ShowSoftWaterEdge": self.softWaterSwitch.on ? @"Yes" : @"No",
        @"ShowTrees": self.showPropsSwitch.on ? @"Yes" : @"No",
        @"TextureFilter": textureFilter,
        @"TextureReduction": [NSString stringWithFormat:@"%ld", (long)textureReduction],
        @"UnitVoices": voices,
        @"UseCloudMap": self.cloudShadowsSwitch.on ? @"Yes" : @"No",
        @"UseLightMap": self.groundLightingSwitch.on ? @"Yes" : @"No",
        @"UseShadowDecals": self.shadow2DSwitch.on ? @"Yes" : @"No",
        @"UseShadowVolumes": self.shadow3DSwitch.on ? @"Yes" : @"No",
        @"WaterEffects": self.zeroHourWaterSwitch.on ? @"Yes" : @"No"
    };

    NSError *zeroHourError = nil;
    BOOL zeroHourOK = WriteKeyValueFile(ZeroHourSettingsPath(), zeroHourValues, &zeroHourError);

    // Options.ini is consumed from the canonical GeneralsX user-data directory.
    // Read that file first so unrelated engine settings are preserved, then mirror
    // the resulting settings to Documents/Options.ini for compatibility with the
    // launcher/game-file layout.
    NSMutableDictionary<NSString *, NSString *> *gameOptions = ReadKeyValueFile(EngineOptionsPath());
    if (gameOptions.count == 0)
        gameOptions = ReadKeyValueFile(GameOptionsPath());
    gameOptions[@"TextureReduction"] = [NSString stringWithFormat:@"%ld", (long)textureReduction];
    gameOptions[@"HeatEffects"] = self.heatEffectsSwitch.on ? @"yes" : @"no";
    gameOptions[@"DynamicLOD"] = self.dynamicLODSwitch.on ? @"yes" : @"no";
    gameOptions[@"UseShadowVolumes"] = self.shadow3DSwitch.on ? @"yes" : @"no";
    gameOptions[@"UseShadowDecals"] = self.shadow2DSwitch.on ? @"yes" : @"no";
    gameOptions[@"UseCloudMap"] = self.cloudShadowsSwitch.on ? @"yes" : @"no";
    gameOptions[@"UseLightMap"] = self.groundLightingSwitch.on ? @"yes" : @"no";
    gameOptions[@"ShowSoftWaterEdge"] = self.softWaterSwitch.on ? @"yes" : @"no";
    gameOptions[@"ShowTrees"] = self.showPropsSwitch.on ? @"yes" : @"no";
    gameOptions[@"ExtraAnimations"] = self.extraAnimationsSwitch.on ? @"yes" : @"no";
    gameOptions[@"BuildingOcclusion"] = self.buildingOcclusionSwitch.on ? @"yes" : @"no";
    gameOptions[@"MaxParticleCount"] = [NSString stringWithFormat:@"%ld", (long)particleCount];
    gameOptions[@"IdealStaticGameLOD"] = @"High";
    gameOptions[@"StaticGameLOD"] = @"Custom";
    gameOptions[@"TextureFilter"] = textureFilter;
    NSArray<NSString *> *anisotropyLevels = @[@"2", @"4", @"8", @"16"];
    gameOptions[@"AnisotropyLevel"] = anisotropyLevels[MAX(0, MIN(3, self.anisotropySegment.selectedSegmentIndex))];
    NSArray<NSString *> *aaLevels = @[@"0", @"2", @"4", @"8"];
    gameOptions[@"AntiAliasing"] = aaLevels[MAX(0, MIN(3, self.antiAliasingSegment.selectedSegmentIndex))];
    gameOptions[@"FogEffects"] = self.zeroHourFogSwitch.on ? @"yes" : @"no";
    gameOptions[@"FPSLimit"] = self.fpsLimitSwitch.on ? @"yes" : @"no";
    gameOptions[@"UseFPSLimit"] = self.fpsLimitSwitch.on ? @"yes" : @"no";
    gameOptions[@"FramesPerSecondLimit"] = [NSString stringWithFormat:@"%.0f", self.fpsSlider.value];
    gameOptions[@"MaxCameraHeight"] = [NSString stringWithFormat:@"%.1f", self.maxCameraSlider.value];
    gameOptions[@"MinCameraHeight"] = [NSString stringWithFormat:@"%.1f", self.minCameraSlider.value];
    gameOptions[@"CameraPitch"] = [NSString stringWithFormat:@"%.1f", self.cameraPitchSlider.value];
    gameOptions[@"TerrainDrawDistanceScale"] = [NSString stringWithFormat:@"%.2f", self.drawDistanceSlider.value];
    gameOptions[@"ScrollFactor"] = [NSString stringWithFormat:@"%ld", lroundf(self.scrollSpeedSlider.value * 100.0f)];

    NSError *optionsError = nil;
    BOOL canonicalOptionsOK = WriteKeyValueFile(EngineOptionsPath(), gameOptions, &optionsError);
    NSError *mirrorOptionsError = nil;
    BOOL mirrorOptionsOK = WriteKeyValueFile(GameOptionsPath(), gameOptions, &mirrorOptionsError);
    BOOL optionsOK = canonicalOptionsOK && mirrorOptionsOK;

    if (iosOK && zeroHourOK && optionsOK)
    {
        self.settingsStatus.text = @"✓ Настройки сохранены. Изменения применятся при следующем запуске игры.";
        self.settingsStatus.textColor = [UIColor colorWithRed:0.18 green:0.88 blue:0.48 alpha:1.0];
        fprintf(stderr, "INFO: iOS launcher settings saved successfully\n");
    }
    else
    {
        NSError *error = iosError != nil ? iosError :
            (zeroHourError != nil ? zeroHourError :
             (optionsError != nil ? optionsError : mirrorOptionsError));
        self.settingsStatus.text = [NSString stringWithFormat:@"✕ Не удалось сохранить настройки: %@",
                                    error.localizedDescription ?: @"неизвестная ошибка"];
        self.settingsStatus.textColor = [UIColor colorWithRed:1.0 green:0.42 blue:0.32 alpha:1.0];
        fprintf(stderr, "ERROR: iOS launcher settings save failed: %s\n",
                error != nil ? error.localizedDescription.UTF8String : "unknown error");
    }
}

- (NSString *)valueForKey:(NSString *)key inContents:(NSString *)contents
{
    NSString *prefix = [key stringByAppendingString:@"="];
    for (NSString *line in [contents componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]])
    {
        NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *compact = [trimmed stringByReplacingOccurrencesOfString:@" " withString:@""];
        if ([compact hasPrefix:prefix])
        {
            NSRange equals = [trimmed rangeOfString:@"="];
            if (equals.location != NSNotFound)
            {
                return [[trimmed substringFromIndex:equals.location + 1]
                        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            }
        }
    }
    return nil;
}

- (float)floatSetting:(NSString *)key contents:(NSString *)contents fallback:(float)fallback
{
    NSString *value = [self valueForKey:key inContents:contents];
    return value.length > 0 ? value.floatValue : fallback;
}

- (BOOL)boolSetting:(NSString *)key contents:(NSString *)contents fallback:(BOOL)fallback
{
    NSString *value = [[self valueForKey:key inContents:contents] lowercaseString];
    if ([value isEqualToString:@"yes"] || [value isEqualToString:@"true"] || [value isEqualToString:@"1"])
        return YES;
    if ([value isEqualToString:@"no"] || [value isEqualToString:@"false"] || [value isEqualToString:@"0"])
        return NO;
    return fallback;
}

- (NSInteger)segmentIndexForValue:(NSString *)value choices:(NSArray<NSString *> *)choices fallback:(NSInteger)fallback
{
    for (NSInteger i = 0; i < (NSInteger)choices.count; ++i)
    {
        if ([value caseInsensitiveCompare:choices[i]] == NSOrderedSame)
            return i;
    }
    return fallback;
}

- (void)resetZeroHourSettingsControls
{
    NSDictionary<NSString *, NSString *> *defaults = DefaultZeroHourSettings();
    self.zeroHourControlBarSegment.selectedSegmentIndex = 0;
    self.zeroHourCameosSegment.selectedSegmentIndex = 0;
    self.zeroHourMusicSegment.selectedSegmentIndex = 0;
    self.zeroHourVoicesSegment.selectedSegmentIndex = 0;
    self.zeroHourHotkeysSegment.selectedSegmentIndex = 0;
    self.zeroHourHotkeyLanguageSegment.selectedSegmentIndex = 0;
    self.zeroHourPortraitsSegment.selectedSegmentIndex = 0;
    self.zeroHourFogSwitch.on = SettingBoolValue(defaults, @"FogEffects", NO);
    self.zeroHourWaterSwitch.on = SettingBoolValue(defaults, @"WaterEffects", YES);
    self.zeroHourExtraBuildingPropsSwitch.on = SettingBoolValue(defaults, @"ExtraBuildingProps", YES);

    self.shadow3DSwitch.on = SettingBoolValue(defaults, @"UseShadowVolumes", NO);
    self.shadow2DSwitch.on = SettingBoolValue(defaults, @"UseShadowDecals", YES);
    self.cloudShadowsSwitch.on = SettingBoolValue(defaults, @"UseCloudMap", NO);
    self.groundLightingSwitch.on = SettingBoolValue(defaults, @"UseLightMap", YES);
    self.softWaterSwitch.on = SettingBoolValue(defaults, @"ShowSoftWaterEdge", YES);
    self.buildingOcclusionSwitch.on = SettingBoolValue(defaults, @"BuildingOcclusion", YES);
    self.showPropsSwitch.on = SettingBoolValue(defaults, @"ShowTrees", YES);
    self.extraAnimationsSwitch.on = SettingBoolValue(defaults, @"ExtraAnimations", YES);
    self.dynamicLODSwitch.on = SettingBoolValue(defaults, @"DynamicLOD", NO);
    self.heatEffectsSwitch.on = SettingBoolValue(defaults, @"HeatEffects", NO);
    self.textureQualitySegment.selectedSegmentIndex = 0;
    self.particleQualitySegment.selectedSegmentIndex = 1;
    self.textureFilterSegment.selectedSegmentIndex = 2;
}

- (void)loadZeroHourSettingsControls
{
    EnsureDefaultZeroHourSettings();
    NSDictionary<NSString *, NSString *> *values = ReadKeyValueFile(ZeroHourSettingsPath());

    self.zeroHourControlBarSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"ControlBar", @"ZeroHour")
                           choices:@[@"ZeroHour", @"Pro", @"Standard"]
                          fallback:0];
    self.zeroHourCameosSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"Cameos", @"Standard")
                           choices:@[@"Standard", @"HD"]
                          fallback:0];
    self.zeroHourMusicSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"Music", @"Standard")
                           choices:@[@"Standard", @"Enhanced", @"The Score"]
                          fallback:0];
    self.zeroHourVoicesSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"UnitVoices", @"English")
                           choices:@[@"English", @"Native"]
                          fallback:0];
    self.zeroHourHotkeysSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"Hotkeys", @"Original")
                           choices:@[@"Original", @"Leikeze"]
                          fallback:0];
    self.zeroHourHotkeyLanguageSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"HotkeyLanguage", @"English")
                           choices:@[@"English", @"Russian"]
                          fallback:0];
    self.zeroHourPortraitsSegment.selectedSegmentIndex =
        [self segmentIndexForValue:SettingValue(values, @"Portraits", @"Standard")
                           choices:@[@"Standard", @"Funny"]
                          fallback:0];

    self.zeroHourFogSwitch.on = SettingBoolValue(values, @"FogEffects", NO);
    self.zeroHourWaterSwitch.on = SettingBoolValue(values, @"WaterEffects", YES);
    self.zeroHourExtraBuildingPropsSwitch.on = SettingBoolValue(values, @"ExtraBuildingProps", YES);

    self.shadow3DSwitch.on = SettingBoolValue(values, @"UseShadowVolumes", NO);
    self.shadow2DSwitch.on = SettingBoolValue(values, @"UseShadowDecals", YES);
    self.cloudShadowsSwitch.on = SettingBoolValue(values, @"UseCloudMap", NO);
    self.groundLightingSwitch.on = SettingBoolValue(values, @"UseLightMap", YES);
    self.softWaterSwitch.on = SettingBoolValue(values, @"ShowSoftWaterEdge", YES);
    self.buildingOcclusionSwitch.on = SettingBoolValue(values, @"BuildingOcclusion", YES);
    self.showPropsSwitch.on = SettingBoolValue(values, @"ShowTrees", YES);
    self.extraAnimationsSwitch.on = SettingBoolValue(values, @"ExtraAnimations", YES);
    self.dynamicLODSwitch.on = SettingBoolValue(values, @"DynamicLOD", NO);
    self.heatEffectsSwitch.on = SettingBoolValue(values, @"HeatEffects", NO);

    NSInteger textureReduction = [SettingValue(values, @"TextureReduction", @"0") integerValue];
    self.textureQualitySegment.selectedSegmentIndex = MAX(0, MIN(2, textureReduction));

    NSInteger particleCount = [SettingValue(values, @"MaxParticleCount", @"2500") integerValue];
    self.particleQualitySegment.selectedSegmentIndex = particleCount <= 1200 ? 0 : (particleCount >= 4000 ? 2 : 1);

    NSString *filter = SettingValue(values, @"TextureFilter", @"Anisotropic");
    self.textureFilterSegment.selectedSegmentIndex =
        [filter caseInsensitiveCompare:@"Bilinear"] == NSOrderedSame ? 0 :
        ([filter caseInsensitiveCompare:@"Trilinear"] == NSOrderedSame ? 1 : 2);
}

- (void)resetНастройки
{
    // Reset the visible controls and immediately persist the defaults to the same
    // Options.ini file consumed by the game. This replaces the old missing selector
    // that caused the launcher to terminate when the button was pressed.
    [self resetНастройкиControls];
    [self saveНастройки];
    self.settingsStatus.text = @"✓ Настройки сброшены и сохранены. Изменения применятся при следующем запуске игры.";
    self.settingsStatus.textColor = [UIColor colorWithRed:0.18 green:0.88 blue:0.48 alpha:1.0];
    fprintf(stderr, "INFO: iOS launcher settings reset to defaults and saved to Options.ini\n");
}

- (void)resetНастройкиControls
{
    [self resetZeroHourSettingsControls];

    self.maxCameraSlider.value = 550.0f;
    self.minCameraSlider.value = 70.0f;
    self.cameraPitchSlider.value = 37.0f;
    self.enforceMaxSwitch.on = NO;
    self.scrollSpeedSlider.value = 1.0f;
    self.drawDistanceSlider.value = 1.35f;
    self.fpsLimitSwitch.on = YES;
    self.fpsSlider.value = 60.0f;
    [self settingsSliderChanged:nil];
    [self fpsLimitChanged:self.fpsLimitSwitch];
}

- (void)loadНастройкиControls
{
    [self loadZeroHourSettingsControls];
    NSDictionary<NSString *, NSString *> *graphics = ReadKeyValueFile(EngineOptionsPath());
    self.shadow3DSwitch.on = SettingBoolValue(graphics, @"UseShadowVolumes", YES);
    self.shadow2DSwitch.on = SettingBoolValue(graphics, @"UseShadowDecals", YES);
    self.cloudShadowsSwitch.on = SettingBoolValue(graphics, @"UseCloudMap", NO);
    self.groundLightingSwitch.on = SettingBoolValue(graphics, @"UseLightMap", YES);
    self.softWaterSwitch.on = SettingBoolValue(graphics, @"ShowSoftWaterEdge", NO);
    self.buildingOcclusionSwitch.on = SettingBoolValue(graphics, @"BuildingOcclusion", YES);
    self.showPropsSwitch.on = SettingBoolValue(graphics, @"ShowTrees", YES);
    self.extraAnimationsSwitch.on = SettingBoolValue(graphics, @"ExtraAnimations", YES);
    self.dynamicLODSwitch.on = SettingBoolValue(graphics, @"DynamicLOD", NO);
    self.heatEffectsSwitch.on = SettingBoolValue(graphics, @"HeatEffects", NO);
    self.textureQualitySegment.selectedSegmentIndex = MAX(0, MIN(2, [SettingValue(graphics, @"TextureReduction", @"0") integerValue]));
    NSInteger particleCount = [SettingValue(graphics, @"MaxParticleCount", @"2500") integerValue];
    self.particleQualitySegment.selectedSegmentIndex = particleCount <= 1200 ? 0 : (particleCount >= 4000 ? 2 : 1);
    NSString *filter = SettingValue(graphics, @"TextureFilter", @"Anisotropic");
    self.textureFilterSegment.selectedSegmentIndex = [filter caseInsensitiveCompare:@"Bilinear"] == NSOrderedSame ? 0 : ([filter caseInsensitiveCompare:@"Trilinear"] == NSOrderedSame ? 1 : 2);
    NSInteger anisotropy = [SettingValue(graphics, @"AnisotropyLevel", @"16") integerValue];
    self.anisotropySegment.selectedSegmentIndex = anisotropy <= 2 ? 0 : (anisotropy <= 4 ? 1 : (anisotropy <= 8 ? 2 : 3));
    NSInteger aa = [SettingValue(graphics, @"AntiAliasing", @"0") integerValue];
    self.antiAliasingSegment.selectedSegmentIndex = aa <= 0 ? 0 : (aa <= 2 ? 1 : (aa <= 4 ? 2 : 3));

    NSError *error = nil;
    NSString *contents = [NSString stringWithContentsOfFile:IOSIPadOverridesPath()
                                                   encoding:NSUTF8StringEncoding
                                                      error:&error];
    if (contents == nil)
    {
        self.maxCameraSlider.value = 550.0f;
        self.minCameraSlider.value = 70.0f;
        self.cameraPitchSlider.value = 37.0f;
        self.enforceMaxSwitch.on = NO;
        self.scrollSpeedSlider.value = 1.0f;
        self.drawDistanceSlider.value = 1.35f;
        self.fpsLimitSwitch.on = YES;
        self.fpsSlider.value = 60.0f;
        self.settingsStatus.text = @"Используются настройки камеры по умолчанию.";
        if (error != nil)
        {
            fprintf(stderr, "WARNING: iOS launcher could not read iOSIPadOverrides.ini: %s\n",
                    [[error description] UTF8String]);
        }
    }
    else
    {
        self.maxCameraSlider.value = [self floatSetting:@"MaxCameraHeight" contents:contents fallback:550.0f];
        self.minCameraSlider.value = [self floatSetting:@"MinCameraHeight" contents:contents fallback:70.0f];
        self.cameraPitchSlider.value = [self floatSetting:@"CameraPitch" contents:contents fallback:37.0f];
        self.enforceMaxSwitch.on = [self boolSetting:@"EnforceMaxCameraHeight" contents:contents fallback:NO];
        self.scrollSpeedSlider.value = [self floatSetting:@"KeyboardScrollSpeedFactor" contents:contents fallback:1.0f];
        self.drawDistanceSlider.value = [self floatSetting:@"TerrainDrawDistanceScale" contents:contents fallback:1.20f];
        self.fpsLimitSwitch.on = [self boolSetting:@"UseFPSLimit" contents:contents fallback:YES];
        self.fpsSlider.value = [self floatSetting:@"FramesPerSecondLimit" contents:contents fallback:60.0f];
        self.settingsStatus.text = @"";
    }

    [self settingsSliderChanged:nil];
    [self fpsLimitChanged:self.fpsLimitSwitch];
}

- (void)showНастройки
{
    [self loadНастройкиControls];
    self.menuStack.hidden = NO;
    self.diagnosticsView.hidden = YES;
    self.profileView.hidden = YES;
    self.modalBackdrop.hidden = NO;
    self.settingsView.hidden = NO;
}

- (void)hideНастройки
{
    self.settingsView.hidden = YES;
    self.modalBackdrop.hidden = YES;
    self.menuStack.hidden = NO;
}

- (void)settingsSliderChanged:(UISlider *)sender
{
    auto snap = [](float value, float step) -> float {
        return roundf(value / step) * step;
    };

    self.maxCameraSlider.value = snap(self.maxCameraSlider.value, 10.0f);
    self.minCameraSlider.value = snap(self.minCameraSlider.value, 5.0f);
    self.cameraPitchSlider.value = snap(self.cameraPitchSlider.value, 1.0f);
    self.scrollSpeedSlider.value = snap(self.scrollSpeedSlider.value, 0.1f);
    self.drawDistanceSlider.value = snap(self.drawDistanceSlider.value, 0.05f);
    self.fpsSlider.value = snap(self.fpsSlider.value, 5.0f);

    self.maxCameraValue.text = [NSString stringWithFormat:@"%.0f", self.maxCameraSlider.value];
    self.minCameraValue.text = [NSString stringWithFormat:@"%.0f", self.minCameraSlider.value];
    self.cameraPitchValue.text = [NSString stringWithFormat:@"%.0f°", self.cameraPitchSlider.value];
    self.scrollSpeedValue.text = [NSString stringWithFormat:@"%.1fx", self.scrollSpeedSlider.value];
    self.drawDistanceValue.text = [NSString stringWithFormat:@"%.2fx", self.drawDistanceSlider.value];
    self.fpsValue.text = [NSString stringWithFormat:@"%.0f", self.fpsSlider.value];
}

- (void)fpsLimitChanged:(UISwitch *)sender
{
    BOOL enabled = self.fpsLimitSwitch.on;
    self.fpsSlider.enabled = enabled;
    self.fpsSlider.alpha = enabled ? 1.0 : 0.35;
    self.fpsValue.alpha = enabled ? 1.0 : 0.35;
}

@end

const char *GeneralsXRunIOSProfileLauncher()
{
    const char *forcedProfile = getenv("GX_LAUNCH_PROFILE");
    if (IsSupportedProfile(forcedProfile))
    {
        strlcpy(gSelectedProfile, forcedProfile, sizeof(gSelectedProfile));
        fprintf(stderr, "INFO: iOS launcher forced profile: %s\\n", gSelectedProfile);
        return gSelectedProfile;
    }

    NSString *autoProfile = BundledAutoLaunchProfile();
    if (autoProfile != nil)
    {
        const char *utf8 = [autoProfile UTF8String];
        strlcpy(gSelectedProfile, utf8, sizeof(gSelectedProfile));

        if (![autoProfile isEqualToString:@"zerohour"])
        {
            fprintf(stderr, "INFO: iOS launcher auto-selected bundled profile: %s\\n",
                    gSelectedProfile);
            return gSelectedProfile;
        }

        fprintf(stderr,
                "[ZEROHOUR-SETTINGS] dedicated ZeroHour launcher shown for settings access\\n");
    }

    gLauncherFinished.store(false, std::memory_order_release);
    if (autoProfile == nil)
        strlcpy(gSelectedProfile, "vanilla", sizeof(gSelectedProfile));

    __block UIWindow *launcherWindow = nil;

    void (^presentLauncher)(void) = ^{
        UIWindowScene *scene = FindActiveWindowScene();
        if (scene != nil)
        {
            launcherWindow = [[UIWindow alloc] initWithWindowScene:scene];
            launcherWindow.frame = scene.coordinateSpace.bounds;
        }
        else
        {
            launcherWindow = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
        }

        launcherWindow.windowLevel = UIWindowLevelNormal + 1.0;
        launcherWindow.rootViewController = [[GXProfileLauncherViewController alloc] init];
        [launcherWindow makeKeyAndVisible];

        fprintf(stderr, "INFO: iOS native launcher presented\\n");
    };

    if ([NSThread isMainThread])
        presentLauncher();
    else
        dispatch_sync(dispatch_get_main_queue(), presentLauncher);

    if ([NSThread isMainThread])
    {
        while (!gLauncherFinished.load(std::memory_order_acquire))
        {
            @autoreleasepool
            {
                [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                                      beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
        }
    }
    else
    {
        while (!gLauncherFinished.load(std::memory_order_acquire))
            usleep(10000);
    }

    void (^dismissLauncher)(void) = ^{
        launcherWindow.hidden = YES;
        launcherWindow.rootViewController = nil;
        launcherWindow = nil;
    };

    if ([NSThread isMainThread])
        dismissLauncher();
    else
        dispatch_sync(dispatch_get_main_queue(), dismissLauncher);

    return gSelectedProfile;
}

#endif