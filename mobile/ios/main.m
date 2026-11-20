/*
 * main.m - a Lube program as an iOS app (UIKit).
 *
 * It runs main.lube (mobile/game/, copied into the app) on a thread of its
 * own, and shows its window (gfx) on the screen: as large as fits, in whole
 * pixels where it can, black around it. Touches go to the program as touch
 * events (the first finger as the mouse too); a hardware keyboard's keys as
 * key events. What it prints goes to Xcode's console.
 *
 * Each frame the program presents becomes an image the main thread puts on
 * a layer (only the latest, if the main thread is behind).
 */
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdatomic.h>

#include "lube_embed.h"
#include "lube_gfx.h"

static _Atomic int win_w, win_h;  /* the program's window (0 when none is open) */
static CALayer *canvas;           /* (the main thread's) */
static UIView *screen;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static CGImageRef pending;        /* a frame not shown yet */

/* ---- the program's window, on the screen ---- */

static void show_pending(void) {
    pthread_mutex_lock(&frame_lock);
    CGImageRef image = pending;
    pending = NULL;
    pthread_mutex_unlock(&frame_lock);
    if (!image) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    canvas.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
}

static void present(void *user, const uint32_t *pixels, int width, int height) {
    (void)user;
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    /* (0xAARRGGBB in memory, little-endian: B, G, R, A) */
    CGImageRef image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, rgb,
                                     kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, provider, NULL,
                                     false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(rgb);
    CGDataProviderRelease(provider);
    CFRelease(data);
    pthread_mutex_lock(&frame_lock);
    int scheduled = pending != NULL;  /* (one on its way: this one takes its place) */
    if (pending) CGImageRelease(pending);
    pending = image;
    pthread_mutex_unlock(&frame_lock);
    if (!scheduled) dispatch_async(dispatch_get_main_queue(), ^{ show_pending(); });
}

static void opened(void *user, int width, int height, const char *title) {
    (void)user, (void)title;
    win_w = width;
    win_h = height;
    dispatch_async(dispatch_get_main_queue(), ^{ [screen setNeedsLayout]; });
}

static void closed(void *user) {
    (void)user;
    win_w = win_h = 0;
}

/* ---- the screen: where the window goes, touches, keys ---- */

/* A hardware keyboard's key (its HID usage) as Lube names it; NULL for others. */
static const char *key_name(long code) {
    static const char *const letters[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                          "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char *const digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char *const fkeys[] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
    static const char *const marks[] = {"minus", "equals", "left_bracket", "right_bracket", "backslash", NULL,
                                        "semicolon", "quote", "backquote", "comma", "period", "slash"};
    if (code >= 4 && code <= 29) return letters[code - 4];
    if (code >= 30 && code <= 39) return digits[code - 30];
    if (code >= 45 && code <= 56) return marks[code - 45];
    if (code >= 58 && code <= 69) return fkeys[code - 58];
    switch (code) {
    case 40: return "enter";
    case 41: return "escape";
    case 42: return "backspace";
    case 43: return "tab";
    case 44: return "space";
    case 57: return "caps_lock";
    case 73: return "insert";
    case 74: return "home";
    case 75: return "page_up";
    case 76: return "delete";
    case 77: return "end";
    case 78: return "page_down";
    case 79: return "right";
    case 80: return "left";
    case 81: return "down";
    case 82: return "up";
    case 224: case 228: return "control";
    case 225: case 229: return "shift";
    case 226: case 230: return "alt";
    case 227: case 231: return "command";
    default: return NULL;
    }
}

@interface LubeView : UIView
@end

@implementation LubeView {
    NSMutableArray *fingers;  /* each touch's place is its id (NSNull: free) */
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = UIColor.blackColor;
        self.multipleTouchEnabled = YES;
        canvas = [CALayer layer];
        canvas.magnificationFilter = kCAFilterNearest;  /* (pixels stay pixels) */
        [self.layer addSublayer:canvas];
        fingers = [NSMutableArray array];
    }
    return self;
}

/* The window on the screen, in the screen's pixels: where it is, and how many each of its pixels takes. */
- (void)fit:(double *)x y:(double *)y scale:(double *)s {
    CGFloat k = self.contentScaleFactor;
    lube_gfx_fit(win_w, win_h, (int)(self.bounds.size.width * k), (int)(self.bounds.size.height * k), x, y, s);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    int w = win_w, h = win_h;
    if (!w) return;
    double x, y, s;
    [self fit:&x y:&y scale:&s];
    CGFloat k = self.contentScaleFactor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    canvas.frame = CGRectMake(x / k, y / k, w * s / k, h * s / k);
    [CATransaction commit];
}

- (void)post:(int)kind touches:(NSSet<UITouch *> *)touches {
    if (!win_w) return;
    double ox, oy, s;
    [self fit:&ox y:&oy scale:&s];
    CGFloat k = self.contentScaleFactor;
    for (UITouch *t in touches) {
        NSUInteger id = [fingers indexOfObjectIdenticalTo:t];
        if (id == NSNotFound) {
            if (kind != LUBE_GFX_TOUCH_DOWN) continue;
            id = [fingers indexOfObject:[NSNull null]];
            if (id == NSNotFound) {
                id = fingers.count;
                [fingers addObject:t];
            } else {
                fingers[id] = t;
            }
        }
        if (kind == LUBE_GFX_TOUCH_UP) fingers[id] = [NSNull null];
        CGPoint p = [t locationInView:self];
        LubeGfxEvent e;
        memset(&e, 0, sizeof e);
        e.kind = kind;
        e.id = (int)id;
        e.x = (p.x * k - ox) / s;
        e.y = (p.y * k - oy) / s;
        lube_gfx_post(&e);
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self post:LUBE_GFX_TOUCH_DOWN touches:touches];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self post:LUBE_GFX_TOUCH_MOVE touches:touches];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self post:LUBE_GFX_TOUCH_UP touches:touches];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self post:LUBE_GFX_TOUCH_UP touches:touches];
}

@end

@interface LubeViewController : UIViewController
@end

@implementation LubeViewController

- (void)loadView {
    screen = [[LubeView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.view = screen;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)canBecomeFirstResponder { return YES; }  /* (a hardware keyboard's keys) */
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}

- (BOOL)postKeys:(NSSet<UIPress *> *)presses down:(BOOL)down {
    BOOL any = NO;
    for (UIPress *press in presses) {
        const char *name = press.key ? key_name((long)press.key.keyCode) : NULL;
        if (!name) continue;
        any = YES;
        LubeGfxEvent e;
        memset(&e, 0, sizeof e);
        e.kind = down ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
        e.key = name;
        lube_gfx_post(&e);
        NSString *typed = press.key.characters;
        if (down && typed.length && [typed characterAtIndex:0] >= 32 && [typed characterAtIndex:0] != 127 &&
            !(press.key.modifierFlags & UIKeyModifierCommand)) {
            memset(&e, 0, sizeof e);
            e.kind = LUBE_GFX_TEXT;
            e.text = typed.UTF8String;
            lube_gfx_post(&e);
        }
    }
    return any;
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self postKeys:presses down:YES]) [super pressesBegan:presses withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self postKeys:presses down:NO]) [super pressesEnded:presses withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self postKeys:presses down:NO]) [super pressesCancelled:presses withEvent:event];
}

@end

/* ---- the program ---- */

static void post_simple(int kind) {
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    lube_gfx_post(&e);
}

static void run_program(void) {
    NSString *path = [NSBundle.mainBundle pathForResource:@"main" ofType:@"lube"];
    if (!path) {
        fprintf(stderr, "lube: there is no main.lube in the app\n");
        return;
    }
    /* (its folder, the app's, is where its files are: load_image("ship.png")) */
    chdir(path.stringByDeletingLastPathComponent.fileSystemRepresentation);
    LubeVM *vm = lube_open();
    if (lube_run_script(vm, path.fileSystemRepresentation) == LUBE_ERROR) fprintf(stderr, "%s\n", lube_error_message(vm));
    lube_close(vm);
    fflush(stdout);
}

@interface LubeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end

@implementation LubeSceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[LubeViewController alloc] init];
    [self.window makeKeyAndVisible];
    static dispatch_once_t once;  /* (one program, however many times a scene connects) */
    dispatch_once(&once, ^{
        LubeGfxHost host;
        memset(&host, 0, sizeof host);
        host.present = present;
        host.opened = opened;
        host.closed = closed;
        lube_gfx_set_host(&host);
        NSThread *thread = [[NSThread alloc] initWithBlock:^{ run_program(); }];
        thread.stackSize = 64 << 20;  /* (as much as the lube command has: deep recursion) */
        [thread start];
    });
}
- (void)sceneDidBecomeActive:(UIScene *)scene { post_simple(LUBE_GFX_FOCUS); }
- (void)sceneWillResignActive:(UIScene *)scene { post_simple(LUBE_GFX_BLUR); }

@end

@interface LubeAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation LubeAppDelegate
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([LubeAppDelegate class]));
    }
}
