#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

#include "IOSGameOverlay.h"

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

static UIButton *s_escButton = nil;
static SDL_Window *s_sdlWindow = nullptr;
static SDL_WindowID s_windowID = 0;
static id s_keyWindowObserver = nil;
static NSUInteger s_escVisibilityGeneration = 0;

static UIWindow *GXFindSDLWindow(void)
{
    if (s_sdlWindow != nullptr) {
        SDL_PropertiesID props = SDL_GetWindowProperties(s_sdlWindow);
        if (props != 0) {
            UIWindow *uiWindow =
                (__bridge UIWindow *)SDL_GetPointerProperty(
                    props,
                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,
                    nullptr);
            if (uiWindow != nil && !uiWindow.hidden) {
                return uiWindow;
            }
        }
    }

    UIApplication *application = UIApplication.sharedApplication;

    for (UIScene *scene in application.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) {
            continue;
        }

        UIWindowScene *windowScene = (UIWindowScene *)scene;
        if (windowScene.activationState != UISceneActivationStateForegroundActive &&
            windowScene.activationState != UISceneActivationStateForegroundInactive) {
            continue;
        }

        for (UIWindow *window in windowScene.windows) {
            if (window.isKeyWindow && !window.hidden) {
                return window;
            }
        }
    }

    return nil;
}

static void GXPushEscapeEvent(bool down)
{
    SDL_Event event;
    SDL_zero(event);

    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = s_windowID;
    event.key.which = 0;
    event.key.scancode = SDL_SCANCODE_ESCAPE;
    event.key.key = SDLK_ESCAPE;
    event.key.mod = SDL_KMOD_NONE;
    event.key.raw = 0;
    event.key.down = down;
    event.key.repeat = false;

    SDL_PushEvent(&event);
}

static void GXShowEscButton(void)
{
    if (s_escButton == nil) {
        return;
    }

    ++s_escVisibilityGeneration;
    s_escButton.alpha = 1.0;
    s_escButton.hidden = NO;

    const NSUInteger generation = s_escVisibilityGeneration;

    // Stay fully visible for about 3 seconds after the last ESC interaction.
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
            if (s_escButton == nil ||
                generation != s_escVisibilityGeneration ||
                s_windowID == 0) {
                return;
            }

            // Fade smoothly from 100% to 0% over 2 seconds.
            [UIView animateWithDuration:2.0
                                  delay:0.0
                                options:UIViewAnimationOptionBeginFromCurrentState |
                                        UIViewAnimationOptionAllowUserInteraction |
                                        UIViewAnimationOptionCurveEaseInOut
                             animations:^{
                if (generation == s_escVisibilityGeneration &&
                    s_escButton != nil) {
                    s_escButton.alpha = 0.0;
                }
            } completion:nil];
        });
}

@interface GXEscButton : UIButton
@end

@implementation GXEscButton

- (void)touchesBegan:(NSSet<UITouch *> *)touches
           withEvent:(UIEvent *)event
{
    [self.layer removeAllAnimations];
    GXShowEscButton();
    self.alpha = 0.65;
    GXPushEscapeEvent(true);
    [super touchesBegan:touches withEvent:event];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches
           withEvent:(UIEvent *)event
{
    [self.layer removeAllAnimations];
    GXShowEscButton();
    self.alpha = 1.0;
    GXPushEscapeEvent(false);
    [super touchesEnded:touches withEvent:event];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches
                withEvent:(UIEvent *)event
{
    [self.layer removeAllAnimations];
    GXShowEscButton();
    self.alpha = 1.0;
    GXPushEscapeEvent(false);
    [super touchesCancelled:touches withEvent:event];
}

@end

static void GXAttachEscButtonToSDLWindow(void)
{
    if (s_windowID == 0) {
        return;
    }

    UIWindow *hostWindow = GXFindSDLWindow();
    if (hostWindow == nil) {
        fprintf(stderr, "WARNING: iOS ESC overlay: SDL UIKit window is not ready yet\n");
        return;
    }

    const CGFloat buttonSize = 50.0;
    const CGFloat left = 5.0;
    const CGFloat top = 25.0;

    if (s_escButton == nil) {
        GXEscButton *button = [GXEscButton buttonWithType:UIButtonTypeSystem];
        button.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.18];
        button.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.78].CGColor;
        button.layer.borderWidth = 1.25;
        button.layer.cornerRadius = 2.0;
        button.clipsToBounds = YES;

        [button setTitle:@"ESC" forState:UIControlStateNormal];
        [button setTitleColor:[UIColor colorWithWhite:1.0 alpha:0.92]
                     forState:UIControlStateNormal];
        button.titleLabel.font =
            [UIFont systemFontOfSize:14.0 weight:UIFontWeightSemibold];
        button.accessibilityLabel = @"Escape";
        button.accessibilityTraits = UIAccessibilityTraitButton;
        button.autoresizingMask =
            UIViewAutoresizingFlexibleRightMargin |
            UIViewAutoresizingFlexibleBottomMargin;

        s_escButton = button;
    }

    s_escButton.frame = CGRectMake(left, top, buttonSize, buttonSize);

    if (s_escButton.superview != hostWindow) {
        [s_escButton removeFromSuperview];
        [hostWindow addSubview:s_escButton];
    }

    [hostWindow bringSubviewToFront:s_escButton];
    GXShowEscButton();

    fprintf(stderr,
            "INFO: iOS in-game ESC overlay attached to SDL UIWindow at x=%.0f y=%.0f size=%.0fx%.0f\n",
            left, top, buttonSize, buttonSize);
}

extern "C" void GeneralsXInstallIOSEscOverlay(SDL_Window *window)
{
    if (window == nullptr) {
        return;
    }

    s_sdlWindow = window;
    s_windowID = SDL_GetWindowID(window);

    dispatch_async(dispatch_get_main_queue(), ^{
        GXAttachEscButtonToSDLWindow();

        // SDL/iOS can finish exposing the UIKit window one run-loop turn later.
        // Retry briefly so the control cannot accidentally attach to the launcher
        // window before the actual game window is ready.
        const double delays[] = {0.10, 0.30, 0.75, 1.50};
        const size_t delayCount = sizeof(delays) / sizeof(delays[0]);

        for (size_t i = 0; i < delayCount; ++i) {
            const double delay = delays[i];
            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                dispatch_get_main_queue(), ^{
                    if (s_windowID != 0) {
                        GXAttachEscButtonToSDLWindow();
                    }
                });
        }

        if (s_keyWindowObserver == nil) {
            s_keyWindowObserver =
                [[NSNotificationCenter defaultCenter]
                    addObserverForName:UIWindowDidBecomeKeyNotification
                                object:nil
                                 queue:[NSOperationQueue mainQueue]
                            usingBlock:^(NSNotification *note) {
                (void)note;
                if (s_windowID != 0) {
                    GXAttachEscButtonToSDLWindow();
                }
            }];
        }
    });
}

extern "C" void GeneralsXRemoveIOSEscOverlay(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_keyWindowObserver != nil) {
            [[NSNotificationCenter defaultCenter]
                removeObserver:s_keyWindowObserver];
            s_keyWindowObserver = nil;
        }

        ++s_escVisibilityGeneration;

        if (s_escButton != nil) {
            [s_escButton.layer removeAllAnimations];
            [s_escButton removeFromSuperview];
            s_escButton = nil;
        }

        s_sdlWindow = nullptr;
        s_windowID = 0;
    });
}

#endif
