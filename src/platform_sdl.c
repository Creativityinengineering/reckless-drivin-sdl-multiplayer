/*
 * platform_sdl.c - SDL2 platform implementation for Reckless Drivin'
 *
 * Replaces the following Classic Mac OS / Carbon APIs with SDL2 equivalents:
 *   - DrawSprocket (screen management, page flipping, gamma fades)
 *   - InputSprocket / HID Manager (keyboard and gamepad input)
 *   - Sound Manager (multi-channel audio mixing)
 *   - DriverServices (high-resolution timing)
 *
 * The original game renders into a 640x480 16-bit (ARGB1555) framebuffer.
 * We maintain a software framebuffer that the game's rendering code writes
 * into directly, then upload it to an SDL streaming texture each frame.
 *
 * Audio uses a simple software mixer: the original Mac Sound Manager channels
 * are emulated as an array of mixing channels, each with independent volume,
 * pan, and pitch control. The SDL audio callback mixes all active channels
 * into the output buffer on the audio thread.
 */

#include "platform.h"
#include "jobs.h"
#include "preferences.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ========================================================================= */
/*  Screen subsystem                                                          */
/* ========================================================================= */

static SDL_Window   *sWindow   = NULL;
static SDL_Renderer *sRenderer = NULL;
static SDL_Texture  *sTexture  = NULL;
static SDL_Texture  *sAIDetailTextures[300];
static int sMultiViewCount;
static int sMultiViewIndex;
static int sWindowViewCount = 1;
static int sUpscaleMode;
static int sRenderScale = 1;
static int sRenderWidth = SCREEN_WIDTH;
static int sRenderHeight = SCREEN_HEIGHT;
typedef struct {
    int spriteIndex;
    float centerX, centerY, direction, zoom;
} tAIDetailDraw;
static tAIDetailDraw sAIDetailDraws[4096];
static int sAIDetailDrawCount;
static int sAIDetailQueueWarning;

/*
 * The game writes 32-bit pixels (see pixel.h) directly into this buffer.
 * Platform_Blit2Screen() uploads it to the GPU texture each frame.
 */
static tPixel sFramebuffer[
    SCREEN_WIDTH * PLATFORM_MAX_RENDER_SCALE *
    (SCREEN_HEIGHT + FRAMEBUFFER_GUARD_ROWS) * PLATFORM_MAX_RENDER_SCALE];

void Platform_InitScreen(void)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[platform] SDL_INIT_VIDEO failed: %s\n",
                SDL_GetError());
        return;
    }

    {
        int winW = gPrefs.unused[0] | (gPrefs.unused[1] << 8);
        int winH = gPrefs.unused[2] | (gPrefs.unused[3] << 8);
        int scale;
        sUpscaleMode = gPrefs.unused[7] ? kPlatformOutput4K :
            (gPrefs.unused[5] ? kPlatformOutput2X : kPlatformOutputOriginal);
        sRenderScale = sUpscaleMode == kPlatformOutput4K ?
            PLATFORM_HQ_RENDER_SCALE : 1;
        sRenderWidth = SCREEN_WIDTH * sRenderScale;
        sRenderHeight = SCREEN_HEIGHT * sRenderScale;
        scale = sUpscaleMode == kPlatformOutput2X ? 2 : 1;
        if (!winW) winW = SCREEN_WIDTH;
        if (!winH) winH = SCREEN_HEIGHT;
        winW *= scale;
        winH *= scale;
        Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (gPrefs.unused[4] || sUpscaleMode == kPlatformOutput4K)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        sWindow = SDL_CreateWindow(
            "Reckless Drivin'",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            winW, winH,
            flags
        );
    }
    if (!sWindow) {
        fprintf(stderr, "[platform] SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        return;
    }

    sRenderer = SDL_CreateRenderer(
        sWindow, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!sRenderer) {
        fprintf(stderr, "[platform] SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        return;
    }

    /* Use crisp pixels by default; the optional upscale mode smooths them. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, sUpscaleMode ? "1" : "0");

    /* Matches the 32-bit framebuffer, so frames upload with a plain copy. */
    Pixel_Init();
    sTexture = SDL_CreateTexture(
        sRenderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        sRenderWidth, sRenderHeight
    );
    if (!sTexture) {
        fprintf(stderr, "[platform] SDL_CreateTexture failed: %s\n",
                SDL_GetError());
        return;
    }
    SDL_SetTextureScaleMode(sTexture, sUpscaleMode ?
        SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    /* Enforce minimum window size (half resolution). */
    SDL_SetWindowMinimumSize(sWindow, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);

    memset(sFramebuffer, 0, sizeof(sFramebuffer));
}

int Platform_GetRenderScale(void)
{
    return sRenderScale;
}

int Platform_GetRenderWidth(void)
{
    return sRenderWidth;
}

int Platform_GetRenderHeight(void)
{
    return sRenderHeight;
}

void Platform_ShutdownScreen(void)
{
    Jobs_Shutdown();
    Platform_ClearAIDetailSprites();
    if (sTexture)  { SDL_DestroyTexture(sTexture);   sTexture  = NULL; }
    if (sRenderer) { SDL_DestroyRenderer(sRenderer);  sRenderer = NULL; }
    if (sWindow)   { SDL_DestroyWindow(sWindow);      sWindow   = NULL; }
}

void Platform_ToggleFullscreen(void)
{
    if (!sWindow) return;
    Uint32 flags = SDL_GetWindowFlags(sWindow);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
        SDL_SetWindowFullscreen(sWindow, 0);
        gPrefs.unused[4] = 0;
    } else {
        /* Save windowed size before going fullscreen */
        int w, h;
        SDL_GetWindowSize(sWindow, &w, &h);
        gPrefs.unused[0] = (UInt8)(w & 0xFF);
        gPrefs.unused[1] = (UInt8)((w >> 8) & 0xFF);
        gPrefs.unused[2] = (UInt8)(h & 0xFF);
        gPrefs.unused[3] = (UInt8)((h >> 8) & 0xFF);
        SDL_SetWindowFullscreen(sWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
        gPrefs.unused[4] = 1;
    }
}

/*
 * Fade brightness: 0 = full black, 255 = full brightness.
 * Applied via SDL_SetTextureColorMod in Blit2Screen.
 */
static UInt8 sFadeBrightness = 255;

/* Forward declaration — defined in input subsystem section below */
static int sQuitRequested;
static void RenderAIDetailSprites(const SDL_Rect *viewport);

/* Time spent inside SDL_RenderPresent (mostly vsync wait), for frame stats. */
static UInt64 sPresentMicros;

static void PresentFrame(void)
{
    UInt64 start = Platform_GetMicroseconds();
    SDL_RenderPresent(sRenderer);
    sPresentMicros += Platform_GetMicroseconds() - start;
}

UInt64 Platform_TakePresentMicroseconds(void)
{
    UInt64 micros = sPresentMicros;
    sPresentMicros = 0;
    return micros;
}

/* RGB555 -> framebuffer pixel, 5-bit channels widened to the full 0..255 range. */
tPixel gPixel555[32768];

void Pixel_Init(void)
{
    int color;
    for (color = 0; color < 32768; color++) {
        UInt32 r = (color >> 10) & 31, g = (color >> 5) & 31, b = color & 31;
        gPixel555[color] = PixelRGB(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
    }
}

void Platform_Blit2Screen(void)
{
    void *texPixels;
    int   texPitch;

    if (!sTexture || !sRenderer)
        return;

    if (SDL_LockTexture(sTexture, NULL, &texPixels, &texPitch) == 0) {
        const int srcPitch = sRenderWidth * (int)sizeof(tPixel);
        if (texPitch == srcPitch) {
            memcpy(texPixels, sFramebuffer, (size_t)sRenderHeight * srcPitch);
        } else {
            const UInt8 *src = (const UInt8 *)sFramebuffer;
            UInt8 *dst = (UInt8 *)texPixels;
            int y;
            for (y = 0; y < sRenderHeight; y++) {
                memcpy(dst, src, srcPitch);
                src += srcPitch;
                dst += texPitch;
            }
        }
        SDL_UnlockTexture(sTexture);
    }

    SDL_SetTextureColorMod(sTexture, sFadeBrightness, sFadeBrightness, sFadeBrightness);
    if (sMultiViewCount > 1) {
        SDL_Rect dst;
        int columns = 2;
        int rows = sMultiViewCount > 2 ? 2 : 1;
        int column = sMultiViewIndex % columns;
        int row = sMultiViewIndex / columns;
        int cellW, cellH, outW, outH;
        float scale;
        SDL_GetRendererOutputSize(sRenderer, &outW, &outH);
        cellW = outW / columns;
        cellH = outH / rows;
        scale = (float)cellW / SCREEN_WIDTH;
        if ((float)cellH / SCREEN_HEIGHT < scale)
            scale = (float)cellH / SCREEN_HEIGHT;
        dst.w = (int)(SCREEN_WIDTH * scale);
        dst.h = (int)(SCREEN_HEIGHT * scale);
        dst.x = column * cellW + (cellW - dst.w) / 2;
        dst.y = row * cellH + (cellH - dst.h) / 2;
        if (SDL_RenderCopy(sRenderer, sTexture, NULL, &dst) < 0)
            fprintf(stderr, "[platform] SDL_RenderCopy failed: %s\n", SDL_GetError());
        RenderAIDetailSprites(&dst);
        sMultiViewIndex++;
        if (sMultiViewIndex >= sMultiViewCount) {
            PresentFrame();
            sMultiViewCount = 0;
            sMultiViewIndex = 0;
        }
    } else {
        Platform_ScaleToFitWindow();
    }
}

void Platform_BeginMultiView(int viewCount)
{
    int columns, rows;
    if (!sRenderer)
        return;
    if (viewCount < 1)
        viewCount = 1;
    sMultiViewCount = viewCount > 4 ? 4 : viewCount;
    sMultiViewIndex = 0;
    sAIDetailDrawCount = 0;
    sAIDetailQueueWarning = 0;
    if (sWindowViewCount != sMultiViewCount) {
        columns = sMultiViewCount > 1 ? 2 : 1;
        rows = sMultiViewCount > 2 ? 2 : 1;
        if (sUpscaleMode != kPlatformOutput4K)
            SDL_SetWindowSize(sWindow, columns * SCREEN_WIDTH *
                (sUpscaleMode == kPlatformOutput2X ? 2 : 1),
                rows * SCREEN_HEIGHT *
                (sUpscaleMode == kPlatformOutput2X ? 2 : 1));
        sWindowViewCount = sMultiViewCount;
    }
    if (sMultiViewCount < 2) {
        sMultiViewCount = 0;
        return;
    }
    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);
}

void Platform_SetUpscaleMode(int outputMode)
{
    int previousMode = sUpscaleMode;
    int requestedRenderScale;
    int scale;
    int columns, rows;
    sUpscaleMode = outputMode < kPlatformOutputOriginal ||
        outputMode > kPlatformOutput4K ? kPlatformOutputOriginal : outputMode;
    requestedRenderScale = sUpscaleMode == kPlatformOutput4K ?
        PLATFORM_HQ_RENDER_SCALE : 1;
    if (requestedRenderScale != sRenderScale)
        fprintf(stderr, "[platform] Internal render resolution changes after restart\n");
    scale = sUpscaleMode == kPlatformOutput2X ? 2 : 1;
    if (sTexture)
        SDL_SetTextureScaleMode(sTexture, sUpscaleMode ?
            SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    if (!sWindow || !sRenderer)
        return;
    if (sUpscaleMode == kPlatformOutput4K) {
        int width, height;
        SDL_GetWindowSize(sWindow, &width, &height);
        gPrefs.unused[0] = (UInt8)(width & 0xff);
        gPrefs.unused[1] = (UInt8)((width >> 8) & 0xff);
        gPrefs.unused[2] = (UInt8)(height & 0xff);
        gPrefs.unused[3] = (UInt8)((height >> 8) & 0xff);
        if (SDL_SetWindowFullscreen(sWindow, SDL_WINDOW_FULLSCREEN_DESKTOP) < 0) {
            fprintf(stderr, "[platform] Could not enable fullscreen HQ output: %s\n",
                SDL_GetError());
            sUpscaleMode = previousMode;
            return;
        }
        SDL_GetRendererOutputSize(sRenderer, &width, &height);
        fprintf(stderr, "[platform] HQ output active at %dx%d (display native resolution)\n",
            width, height);
        return;
    }
    if (previousMode == kPlatformOutput4K) {
        Uint32 flags = gPrefs.unused[4] ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
        if (SDL_SetWindowFullscreen(sWindow, flags) < 0)
            fprintf(stderr, "[platform] Could not leave fullscreen HQ output: %s\n",
                SDL_GetError());
    }
    columns = sWindowViewCount > 1 ? 2 : 1;
    rows = sWindowViewCount > 2 ? 2 : 1;
    SDL_SetWindowSize(sWindow, columns * SCREEN_WIDTH * scale,
        rows * SCREEN_HEIGHT * scale);
}

void Platform_SetAIDetailSprite(int id, int width, int height,
                                const tPixel *pixels)
{
    SDL_Texture *texture;
    SDL_PixelFormat *format;
    UInt32 *rgba;
    size_t pixelCount;
    size_t i;
    int index = id - 128;
    if (!sRenderer || index < 0 || index >= 300 || !pixels ||
        width <= 0 || height <= 0)
        return;
    format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!format) {
        fprintf(stderr, "[platform] Could not allocate AI sprite pixel format: %s\n",
            SDL_GetError());
        return;
    }
    pixelCount = (size_t)width * height;
    rgba = (UInt32 *)malloc(pixelCount * sizeof(*rgba));
    if (!rgba) {
        fprintf(stderr, "[platform] Could not allocate AI sprite texture pixels\n");
        SDL_FreeFormat(format);
        return;
    }
    for (i = 0; i < pixelCount; ++i) {
        tPixel color = pixels[i];
        rgba[i] = SDL_MapRGBA(format, (UInt8)(color >> 16), (UInt8)(color >> 8),
            (UInt8)color, (UInt8)(color >> 24));
    }
    SDL_FreeFormat(format);
    texture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        fprintf(stderr, "[platform] Could not create AI sprite texture: %s\n",
            SDL_GetError());
        free(rgba);
        return;
    }
    if (SDL_UpdateTexture(texture, NULL, rgba, width * (int)sizeof(*rgba)) < 0) {
        fprintf(stderr, "[platform] Could not upload AI sprite texture: %s\n",
            SDL_GetError());
        SDL_DestroyTexture(texture);
        free(rgba);
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    free(rgba);
    if (sAIDetailTextures[index])
        SDL_DestroyTexture(sAIDetailTextures[index]);
    sAIDetailTextures[index] = texture;
}

void Platform_ClearAIDetailSprites(void)
{
    int i;
    for (i = 0; i < 300; ++i) {
        if (sAIDetailTextures[i]) {
            SDL_DestroyTexture(sAIDetailTextures[i]);
            sAIDetailTextures[i] = NULL;
        }
    }
    sAIDetailDrawCount = 0;
}

void Platform_QueueAIDetailSprite(int id, float centerX, float centerY,
                                  float direction, float zoom)
{
    int index = id - 128;
    if (!sUpscaleMode || sRenderScale > 1 || index < 0 || index >= 300 ||
        !sAIDetailTextures[index])
        return;
    if (sAIDetailDrawCount >= (int)(sizeof(sAIDetailDraws) / sizeof(sAIDetailDraws[0]))) {
        if (!sAIDetailQueueWarning) {
            fprintf(stderr, "[platform] AI sprite overlay queue is full for this frame\n");
            sAIDetailQueueWarning = 1;
        }
        return;
    }
    sAIDetailDraws[sAIDetailDrawCount++] = (tAIDetailDraw){
        index, centerX, centerY, direction, zoom
    };
}

static void RenderAIDetailSprites(const SDL_Rect *viewport)
{
    float scale = (float)viewport->w / sRenderWidth;
    int i;
    if (sUpscaleMode) {
        SDL_RenderSetClipRect(sRenderer, viewport);
        for (i = 0; i < sAIDetailDrawCount; ++i) {
            const tAIDetailDraw *draw = &sAIDetailDraws[i];
            SDL_Texture *sprite = sAIDetailTextures[draw->spriteIndex];
            if (sprite) {
                int width, height;
                SDL_Rect target;
                SDL_QueryTexture(sprite, NULL, NULL, &width, &height);
                target.w = (int)(width * draw->zoom * scale / 4.0f + 0.5f);
                target.h = (int)(height * draw->zoom * scale / 4.0f + 0.5f);
                target.x = viewport->x +
                    (int)(draw->centerX * scale - target.w / 2.0f + 0.5f);
                target.y = viewport->y +
                    (int)(draw->centerY * scale - target.h / 2.0f + 0.5f);
                SDL_SetTextureColorMod(sprite, sFadeBrightness,
                    sFadeBrightness, sFadeBrightness);
                if (SDL_RenderCopyEx(sRenderer, sprite, NULL, &target,
                    draw->direction * (180.0 / M_PI), NULL, SDL_FLIP_NONE) < 0)
                    fprintf(stderr, "[platform] Could not draw AI sprite: %s\n",
                        SDL_GetError());
            }
        }
        SDL_RenderSetClipRect(sRenderer, NULL);
    }
    sAIDetailDrawCount = 0;
}

void Platform_ScaleToFitWindow(void)
{
    if (!sRenderer || !sTexture) return;
    SDL_RenderClear(sRenderer);

    /* Scale to fit window while preserving 4:3 aspect ratio (letterbox if needed) */
    int outW, outH;
    SDL_GetRendererOutputSize(sRenderer, &outW, &outH);
    float scaleX = (float)outW / sRenderWidth;
    float scaleY = (float)outH / sRenderHeight;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    SDL_Rect dst;
    dst.w = (int)(sRenderWidth  * scale);
    dst.h = (int)(sRenderHeight * scale);
    dst.x = (outW - dst.w) / 2;
    dst.y = (outH - dst.h) / 2;

    SDL_RenderCopy(sRenderer, sTexture, NULL, &dst);
    RenderAIDetailSprites(&dst);
    PresentFrame();
}

void Platform_FadeScreen(int fade)
{
    /*
     * Original DrawSprocket gamma fading semantics:
     *   fade==1   -> animated fade to black
     *   fade==0   -> animated fade from black (full brightness)
     *   fade>=256 -> instant set: brightness = (fade-256)*255/256
     *   fade>512  -> instant set to full black
     */
    if (fade >= 256) {
        /* Instant brightness set */
        if (fade > 512) {
            sFadeBrightness = 0;
        } else {
            int b = ((fade - 256) * 255) / 256;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            sFadeBrightness = (UInt8)b;
        }
        return;
    }

    /* Animated fade */
    int target = (fade == 1) ? 0 : 255;
    int start  = (int)sFadeBrightness;
    int steps  = 20; /* ~300ms at 16ms per step */

    for (int i = 1; i <= steps; i++) {
        Platform_PollEvents();
        if (sQuitRequested) break;

        sFadeBrightness = (UInt8)(start + (target - start) * i / steps);
        Platform_Blit2Screen();
        SDL_Delay(16);
    }
    sFadeBrightness = (UInt8)target;
}

void Platform_ScreenClear(void)
{
    memset(sFramebuffer, 0, sizeof(sFramebuffer));
}

tPixel *Platform_GetFramebuffer(int *rowBytes)
{
    if (rowBytes)
        *rowBytes = sRenderWidth * (int)sizeof(tPixel);
    return sFramebuffer;
}

/* ========================================================================= */
/*  Input subsystem                                                           */
/* ========================================================================= */

/*
 * We maintain two parallel input mechanisms:
 *   1. Keyboard: an array mapping each game element to an SDL scancode.
 *   2. Gamepad:  optional SDL_GameController support (first connected pad).
 *
 * Platform_GetElement() checks both and returns true if either source is
 * active for the requested element.
 *
 * Platform_GetEvent() tracks per-element state transitions (up->down and
 * down->up) and returns them one at a time, emulating InputSprocket's
 * event list behavior.
 */

/* Scancode assigned to each game element (keyboard mapping). */
static SDL_Scancode sElementKeys[kNumElements];

/* Previous element state for edge detection in GetEvent(). */
static int sElementPrevState[kNumElements];

/* sQuitRequested is forward-declared above (near screen subsystem) for use by FadeScreen */

/* Mouse click tracking (edge-triggered: set on BUTTONDOWN, cleared after read). */
static int sMouseClicked = 0;
static int sMouseClickX  = 0;
static int sMouseClickY  = 0;

/* Key press tracking (edge-triggered: set on KEYDOWN, cleared after read).
 * sKeyPressed records physical presses only; sKeyTyped also records OS
 * auto-repeat. Kept separate so confirm keys can never fire from a key
 * that was already held when a screen opened, while navigation keys still
 * get hold-to-scroll behavior. */
static Uint8 sKeyPressed[SDL_NUM_SCANCODES];
static Uint8 sKeyTyped[SDL_NUM_SCANCODES];

/* Optional gamepad. */
static SDL_GameController *sGameController = NULL;

/* Map game elements to SDL_GameController buttons. */
static const SDL_GameControllerButton sElementPadButton[kNumElements] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP,      /* kForward  */
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,    /* kBackward */
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,    /* kLeft     */
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,   /* kRight    */
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, /* kKickdown */
    SDL_CONTROLLER_BUTTON_A,            /* kBrake    */
    SDL_CONTROLLER_BUTTON_X,            /* kFire     */
    SDL_CONTROLLER_BUTTON_Y,            /* kMissile  */
    SDL_CONTROLLER_BUTTON_BACK,         /* kAbort    */
    SDL_CONTROLLER_BUTTON_START         /* kPause    */
};

static void TryOpenGameController(void)
{
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            sGameController = SDL_GameControllerOpen(i);
            if (sGameController) {
                fprintf(stderr, "[platform] Opened game controller: %s\n",
                        SDL_GameControllerName(sGameController));
                break;
            }
        }
    }
}

void Platform_InitInput(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[platform] SDL_INIT_GAMECONTROLLER failed: %s\n",
                SDL_GetError());
        /* Non-fatal; keyboard still works. */
    }

    /* Set default keyboard mapping. */
    sElementKeys[kForward]   = SDL_SCANCODE_UP;
    sElementKeys[kBackward]  = SDL_SCANCODE_DOWN;
    sElementKeys[kLeft]      = SDL_SCANCODE_LEFT;
    sElementKeys[kRight]     = SDL_SCANCODE_RIGHT;
    sElementKeys[kKickdown]  = SDL_SCANCODE_LSHIFT;
    sElementKeys[kBrake]     = SDL_SCANCODE_SPACE;
    sElementKeys[kFire]      = SDL_SCANCODE_Z;
    sElementKeys[kMissile]   = SDL_SCANCODE_X;
    sElementKeys[kAbort]     = SDL_SCANCODE_ESCAPE;
    sElementKeys[kPause]     = SDL_SCANCODE_P;

    memset(sElementPrevState, 0, sizeof(sElementPrevState));
    sQuitRequested = 0;

    /* Try to open a game controller if one is already connected. */
    TryOpenGameController();
}

void Platform_LoadKeyBindings(const UInt8 *keyCodes)
{
    /*
     * Load saved SDL scancodes from preferences.
     * Called after LoadPrefs() in main.c Init().
     * Only apply if the values look like valid SDL scancodes (> 0 and < 512).
     */
    for (int i = 0; i < kNumElements - 2; i++) { /* skip kAbort and kPause */
        SDL_Scancode sc = (SDL_Scancode)keyCodes[i];
        if (sc > 0 && sc < SDL_NUM_SCANCODES)
            sElementKeys[i] = sc;
    }
}

void Platform_ShutdownInput(void)
{
    if (sGameController) {
        SDL_GameControllerClose(sGameController);
        sGameController = NULL;
    }
}

/*
 * Convert window-space mouse coordinates to 640x480 logical coordinates.
 * Accounts for letterboxing from SDL_RenderSetLogicalSize and window resizing.
 */
static void WindowToLogical(int winX, int winY, int *logX, int *logY)
{
    int winW, winH;
    if (!sWindow) { *logX = winX; *logY = winY; return; }

    SDL_GetWindowSize(sWindow, &winW, &winH);
    if (winW <= 0 || winH <= 0) { *logX = winX; *logY = winY; return; }

    float scaleX = (float)winW / SCREEN_WIDTH;
    float scaleY = (float)winH / SCREEN_HEIGHT;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    int viewW   = (int)(SCREEN_WIDTH  * scale);
    int viewH   = (int)(SCREEN_HEIGHT * scale);
    int offsetX = (winW - viewW) / 2;
    int offsetY = (winH - viewH) / 2;

    *logX = (int)((winX - offsetX) / scale);
    *logY = (int)((winY - offsetY) / scale);
}

/* Open/close the active game controller on hotplug. Shared by the event
 * pump and the modal key-wait loop so a device change is never lost. */
static void HandleControllerDeviceEvent(const SDL_Event *ev)
{
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        if (!sGameController) {
            sGameController = SDL_GameControllerOpen(ev->cdevice.which);
            if (sGameController)
                fprintf(stderr, "[platform] Controller added: %s\n",
                        SDL_GameControllerName(sGameController));
        }
    } else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        if (sGameController &&
            ev->cdevice.which == SDL_JoystickInstanceID(
                SDL_GameControllerGetJoystick(sGameController))) {
            SDL_GameControllerClose(sGameController);
            sGameController = NULL;
            fprintf(stderr, "[platform] Controller removed\n");
        }
    }
}

void Platform_PollEvents(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            sQuitRequested = 1;
            break;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED && sWindow) {
                Platform_ScaleToFitWindow();
                /* Save windowed size (not during fullscreen) */
                if (!(SDL_GetWindowFlags(sWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                    int w = ev.window.data1, h = ev.window.data2;
                    gPrefs.unused[0] = (UInt8)(w & 0xFF);
                    gPrefs.unused[1] = (UInt8)((w >> 8) & 0xFF);
                    gPrefs.unused[2] = (UInt8)(h & 0xFF);
                    gPrefs.unused[3] = (UInt8)((h >> 8) & 0xFF);
                }
            }
            break;

        case SDL_KEYDOWN:
            /* Fullscreen shortcuts are handled here and swallowed so the
             * Enter of Alt+Enter never surfaces as a menu key press. */
            if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                ((ev.key.keysym.mod & KMOD_ALT) && ev.key.keysym.scancode == SDL_SCANCODE_RETURN)) {
                if (!ev.key.repeat)
                    Platform_ToggleFullscreen();
                break;
            }
            if (ev.key.keysym.scancode < SDL_NUM_SCANCODES) {
                if (!ev.key.repeat)
                    sKeyPressed[ev.key.keysym.scancode] = 1;
                sKeyTyped[ev.key.keysym.scancode] = 1;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                int lx, ly;
                WindowToLogical(ev.button.x, ev.button.y, &lx, &ly);
                sMouseClicked = 1;
                sMouseClickX  = lx;
                sMouseClickY  = ly;
            }
            break;

        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            HandleControllerDeviceEvent(&ev);
            break;

        default:
            break;
        }
    }
}

int Platform_IsKeyDown(int scancode)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES)
        return state[scancode] ? 1 : 0;
    return 0;
}

int Platform_WasKeyPressed(int scancode)
{
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES && sKeyPressed[scancode]) {
        sKeyPressed[scancode] = 0;
        return 1;
    }
    return 0;
}

int Platform_WasKeyTyped(int scancode)
{
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES && sKeyTyped[scancode]) {
        sKeyTyped[scancode] = 0;
        return 1;
    }
    return 0;
}

int Platform_GetElement(int element)
{
    if (element < 0 || element >= kNumElements)
        return 0;

    /* Check keyboard. */
    const Uint8 *kbState = SDL_GetKeyboardState(NULL);
    if (kbState[sElementKeys[element]])
        return 1;

    /* Check gamepad. */
    if (sGameController) {
        if (SDL_GameControllerGetButton(sGameController,
                                        sElementPadButton[element]))
            return 1;

        /*
         * Also support analog stick for steering/throttle:
         *   Left stick Y axis: forward (up) / backward (down)
         *   Left stick X axis: left / right
         * Use a dead zone of ~30% (roughly 9830 out of 32767).
         */
        const int kDeadZone = 9830;
        switch (element) {
        case kForward: {
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_LEFTY);
            if (val < -kDeadZone) return 1;
            break;
        }
        case kBackward: {
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_LEFTY);
            if (val > kDeadZone) return 1;
            break;
        }
        case kLeft: {
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_LEFTX);
            if (val < -kDeadZone) return 1;
            break;
        }
        case kRight: {
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_LEFTX);
            if (val > kDeadZone) return 1;
            break;
        }
        case kKickdown: {
            /* Right trigger as kickdown alternative. */
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            if (val > kDeadZone) return 1;
            break;
        }
        case kBrake: {
            /* Left trigger as brake alternative. */
            int val = SDL_GameControllerGetAxis(sGameController,
                          SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            if (val > kDeadZone) return 1;
            break;
        }
        default:
            break;
        }
    }

    return 0;
}

int Platform_GetEvent(int *element, int *data)
{
    /*
     * Scan all elements looking for a state change since the last call.
     * This mimics InputSprocket's ISpElementList_GetNextEvent():
     *   - element: which game element changed
     *   - data: 1 = pressed (down), 0 = released (up)
     *
     * We return one event per call, cycling through elements in order.
     * The caller loops until we return 0 (no more events).
     */
    for (int i = 0; i < kNumElements; i++) {
        int current = Platform_GetElement(i);
        if (current != sElementPrevState[i]) {
            *element = i;
            *data = current;
            sElementPrevState[i] = current;
            return 1;
        }
    }
    return 0;
}

void Platform_FlushInput(void)
{
    /* Discard pending keyboard, text and mouse input only. A full flush
     * (SDL_FIRSTEVENT..SDL_LASTEVENT) also dropped SDL_QUIT and controller
     * hotplug events; losing a CONTROLLERDEVICEREMOVED left the pad handle
     * stale so it could never be reopened for the rest of the session. */
    SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYMAPCHANGED);
    SDL_FlushEvents(SDL_MOUSEMOTION, SDL_MOUSEWHEEL);

    /* Reset edge-detection state so no spurious events fire next frame. */
    for (int i = 0; i < kNumElements; i++)
        sElementPrevState[i] = Platform_GetElement(i);

    /* Clear any pending mouse click and unconsumed key presses. */
    sMouseClicked = 0;
    memset(sKeyPressed, 0, sizeof(sKeyPressed));
    memset(sKeyTyped, 0, sizeof(sKeyTyped));
}

int Platform_ShouldQuit(void)
{
    return sQuitRequested;
}

/* ========================================================================= */
/*  Key mapping API                                                           */
/* ========================================================================= */

void Platform_SetElementKey(int element, SDL_Scancode key)
{
    if (element >= 0 && element < kNumElements)
        sElementKeys[element] = key;
}

SDL_Scancode Platform_GetElementKey(int element)
{
    if (element >= 0 && element < kNumElements)
        return sElementKeys[element];
    return SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode Platform_WaitForKey(void)
{
    /* Block until a non-modifier key is pressed. Return 0 if ESC pressed.
     * Flush only pending keyboard input so a queued SDL_QUIT is not lost. */
    SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYMAPCHANGED);
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                sQuitRequested = 1;
                return SDL_SCANCODE_UNKNOWN;
            }
            if (ev.type == SDL_CONTROLLERDEVICEADDED ||
                ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                HandleControllerDeviceEvent(&ev);
                continue;
            }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Scancode sc = ev.key.keysym.scancode;
                /* Ignore auto-repeat: a key held since before this prompt
                 * opened must not bind itself. */
                if (ev.key.repeat)
                    continue;
                /* Ignore pure modifier keys */
                if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT ||
                    sc == SDL_SCANCODE_LCTRL  || sc == SDL_SCANCODE_RCTRL  ||
                    sc == SDL_SCANCODE_LALT   || sc == SDL_SCANCODE_RALT   ||
                    sc == SDL_SCANCODE_LGUI   || sc == SDL_SCANCODE_RGUI)
                    continue;
                if (sc == SDL_SCANCODE_ESCAPE)
                    return SDL_SCANCODE_UNKNOWN; /* cancelled */
                return sc;
            }
        }
        SDL_Delay(16);
    }
}

/* ========================================================================= */
/*  Audio subsystem                                                           */
/* ========================================================================= */

/*
 * Software mixer for the SDL port.  The original game uses Mac Sound Manager
 * with various sample formats (8-bit unsigned and 16-bit signed big-endian)
 * at various sample rates (11 kHz, 22 kHz, 44.1 kHz).
 *
 * We output signed 16-bit stereo at 44100 Hz.  Each channel stores its
 * source format (8-bit or 16-bit) and a base pitch that encodes the ratio
 * of the source sample rate to 44100 Hz.  The game's pitch multiplier is
 * applied on top of that base.
 *
 * Thread safety: sMixMutex protects all shared channel state.
 */

#define AUDIO_BUFFER_FRAMES 1024

#define FRAC_BITS 16
#define FRAC_ONE  (1 << FRAC_BITS)

typedef struct {
    const UInt8 *sampleData;  /* pointer to PCM data (8-bit or 16-bit BE)  */
    int          sampleLen;   /* length in sample frames                    */
    UInt32       position;    /* playback position in fixed-point           */
    float        volumeL;     /* left channel volume  [0..1]               */
    float        volumeR;     /* right channel volume [0..1]               */
    float        basePitch;   /* native rate / mixer rate (set at play)    */
    float        gamePitch;   /* game-controlled pitch multiplier          */
    int          bitsPerSample; /* 8 or 16                                 */
    int          active;      /* non-zero if channel is playing            */
    int          looping;     /* non-zero to loop the sample               */
} AudioChannel;

static AudioChannel      sChannels[kNumAudioChannels];
static SDL_AudioDeviceID  sAudioDevice = 0;
static SDL_mutex         *sMixMutex    = NULL;

/*
 * Read one sample from a channel and return it as a signed 16-bit value.
 * Handles both unsigned 8-bit and signed 16-bit big-endian source formats.
 */
static inline int ReadSample(const AudioChannel *c, UInt32 idx)
{
    if (c->bitsPerSample == 16) {
        /* Signed 16-bit big-endian */
        const UInt8 *p = c->sampleData + idx * 2;
        int16_t s = (int16_t)((p[0] << 8) | p[1]);
        return (int)s;
    } else {
        /* Unsigned 8-bit → signed 16-bit */
        return ((int)c->sampleData[idx] - 128) << 8;
    }
}

/*
 * SDL audio callback.  Mixes all active channels into a signed 16-bit
 * stereo interleaved output buffer.
 */
static void AudioCallback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;

    /* len is in bytes; each stereo frame is 4 bytes (2 × int16) */
    int numFrames = len / 4;
    int16_t *out = (int16_t *)stream;

    /* Silence */
    memset(stream, 0, len);

    if (SDL_LockMutex(sMixMutex) != 0)
        return;

    for (int ch = 0; ch < kNumAudioChannels; ch++) {
        AudioChannel *c = &sChannels[ch];
        if (!c->active || !c->sampleData)
            continue;

        float volL = c->volumeL;
        float volR = c->volumeR;

        if (volL <= 0.0f && volR <= 0.0f) {
            /* Muted -- advance position silently */
            float pitch = c->basePitch * c->gamePitch;
            if (pitch <= 0.0f) pitch = 1.0f;
            UInt32 pitchFP = (UInt32)(pitch * FRAC_ONE);
            c->position += pitchFP * (UInt32)numFrames;
            if ((c->position >> FRAC_BITS) >= (UInt32)c->sampleLen) {
                if (c->looping)
                    c->position = 0;
                else
                    c->active = 0;
            }
            continue;
        }

        float pitch = c->basePitch * c->gamePitch;
        if (pitch <= 0.0f) pitch = 1.0f;
        UInt32 pitchFP = (UInt32)(pitch * FRAC_ONE);

        /* Scale volumes to 16-bit range (×32767) as fixed-point for speed */
        int iVolL = (int)(volL * 32767.0f);
        int iVolR = (int)(volR * 32767.0f);

        for (int i = 0; i < numFrames; i++) {
            UInt32 sampleIndex = c->position >> FRAC_BITS;
            if (sampleIndex >= (UInt32)c->sampleLen) {
                if (c->looping) {
                    c->position = 0;
                    sampleIndex = 0;
                } else {
                    c->active = 0;
                    break;
                }
            }

            int sample = ReadSample(c, sampleIndex);

            /* Mix into stereo output with volume scaling */
            int outIdx = i * 2;
            int mixL = (int)out[outIdx]     + ((sample * iVolL) >> 15);
            int mixR = (int)out[outIdx + 1] + ((sample * iVolR) >> 15);

            /* Clamp to int16 range */
            if (mixL >  32767) mixL =  32767;
            if (mixL < -32768) mixL = -32768;
            if (mixR >  32767) mixR =  32767;
            if (mixR < -32768) mixR = -32768;

            out[outIdx]     = (int16_t)mixL;
            out[outIdx + 1] = (int16_t)mixR;

            c->position += pitchFP;
        }
    }

    SDL_UnlockMutex(sMixMutex);
}

void Platform_InitAudio(void)
{
    SDL_AudioSpec desired, obtained;

    /* Guard against double initialization */
    if (sAudioDevice != 0)
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[platform] SDL_INIT_AUDIO failed: %s\n",
                SDL_GetError());
        return;
    }

    sMixMutex = SDL_CreateMutex();
    if (!sMixMutex) {
        fprintf(stderr, "[platform] SDL_CreateMutex failed: %s\n",
                SDL_GetError());
        return;
    }

    memset(sChannels, 0, sizeof(sChannels));
    for (int i = 0; i < kNumAudioChannels; i++) {
        sChannels[i].basePitch = 1.0f;
        sChannels[i].gamePitch = 1.0f;
    }

    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq     = AUDIO_MIX_RATE;
    desired.format   = AUDIO_S16SYS;  /* signed 16-bit native endian */
    desired.channels = 2;             /* stereo output */
    desired.samples  = AUDIO_BUFFER_FRAMES;
    desired.callback = AudioCallback;
    desired.userdata = NULL;

    sAudioDevice = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (sAudioDevice == 0) {
        fprintf(stderr, "[platform] SDL_OpenAudioDevice failed: %s\n",
                SDL_GetError());
        return;
    }

    fprintf(stderr, "[platform] Audio: %d Hz, %d-bit, %d ch, %d samples\n",
            obtained.freq, SDL_AUDIO_BITSIZE(obtained.format),
            obtained.channels, obtained.samples);

    SDL_PauseAudioDevice(sAudioDevice, 0);
}

void Platform_ShutdownAudio(void)
{
    if (sAudioDevice) {
        SDL_PauseAudioDevice(sAudioDevice, 1);
        SDL_CloseAudioDevice(sAudioDevice);
        sAudioDevice = 0;
    }
    if (sMixMutex) {
        SDL_DestroyMutex(sMixMutex);
        sMixMutex = NULL;
    }
    memset(sChannels, 0, sizeof(sChannels));
}

void Platform_PlaySound(int channelId, const void *sampleData, int sampleLen,
                        float volume, float pan, float pitch,
                        int bitsPerSample)
{
    if (channelId < 0 || channelId >= kNumAudioChannels)
        return;
    if (!sampleData || sampleLen <= 0)
        return;

    float clampedPan = pan;
    if (clampedPan < -1.0f) clampedPan = -1.0f;
    if (clampedPan >  1.0f) clampedPan =  1.0f;

    float volL = volume * (1.0f - clampedPan) * 0.5f;
    float volR = volume * (1.0f + clampedPan) * 0.5f;

    if (volL < 0.0f) volL = 0.0f; if (volL > 1.0f) volL = 1.0f;
    if (volR < 0.0f) volR = 0.0f; if (volR > 1.0f) volR = 1.0f;

    if (pitch <= 0.0f) pitch = 1.0f;
    if (bitsPerSample != 8 && bitsPerSample != 16)
        bitsPerSample = 8;

    SDL_LockMutex(sMixMutex);
    {
        AudioChannel *c = &sChannels[channelId];
        c->sampleData   = (const UInt8 *)sampleData;
        c->sampleLen    = sampleLen;
        c->position     = 0;
        c->volumeL      = volL;
        c->volumeR      = volR;
        c->basePitch    = pitch;
        c->gamePitch    = 1.0f;
        c->bitsPerSample = bitsPerSample;
        c->active       = 1;
        c->looping      = (channelId == kAudioChannelEngine ||
                           channelId == kAudioChannelSkid) ? 1 : 0;
    }
    SDL_UnlockMutex(sMixMutex);
}

void Platform_StopChannel(int channelId)
{
    if (channelId < 0 || channelId >= kNumAudioChannels)
        return;

    SDL_LockMutex(sMixMutex);
    sChannels[channelId].active = 0;
    SDL_UnlockMutex(sMixMutex);
}

int Platform_IsChannelActive(int channelId)
{
    if (channelId < 0 || channelId >= kNumAudioChannels)
        return 0;
    int active;
    SDL_LockMutex(sMixMutex);
    active = sChannels[channelId].active;
    SDL_UnlockMutex(sMixMutex);
    return active;
}

void Platform_SetChannelVolume(int channelId, float left, float right)
{
    if (channelId < 0 || channelId >= kNumAudioChannels)
        return;

    if (left  < 0.0f) left  = 0.0f; if (left  > 1.0f) left  = 1.0f;
    if (right < 0.0f) right = 0.0f; if (right > 1.0f) right = 1.0f;

    SDL_LockMutex(sMixMutex);
    sChannels[channelId].volumeL = left;
    sChannels[channelId].volumeR = right;
    SDL_UnlockMutex(sMixMutex);
}

void Platform_SetChannelPitch(int channelId, float pitch)
{
    if (channelId < 0 || channelId >= kNumAudioChannels)
        return;

    if (pitch <= 0.0f) pitch = 1.0f;

    SDL_LockMutex(sMixMutex);
    sChannels[channelId].gamePitch = pitch;
    SDL_UnlockMutex(sMixMutex);
}

/* ========================================================================= */
/*  Timing                                                                    */
/* ========================================================================= */

UInt64 Platform_GetMicroseconds(void)
{
    /*
     * SDL_GetPerformanceCounter / SDL_GetPerformanceFrequency gives us
     * the highest resolution monotonic timer available on the platform.
     * We convert to microseconds, being careful to avoid overflow by
     * dividing before multiplying when the frequency is large enough.
     *
     * This replaces the original game's use of DriverServices UpTime()
     * and InputSprocket ISpUptime()/ISpTimeToMicroseconds().
     */
    UInt64 counter = (UInt64)SDL_GetPerformanceCounter();
    UInt64 freq    = (UInt64)SDL_GetPerformanceFrequency();

    /*
     * If the frequency is a multiple of 1,000,000 we can divide it down
     * cleanly to avoid intermediate overflow.  Otherwise, use the general
     * formula which is safe as long as the counter value hasn't been running
     * for centuries (which it hasn't -- SDL counters reset on boot).
     */
    if (freq >= 1000000ULL && (freq % 1000000ULL) == 0) {
        return counter / (freq / 1000000ULL);
    }
    return (counter * 1000000ULL) / freq;
}

/* ========================================================================= */
/*  Mouse                                                                     */
/* ========================================================================= */

void Platform_GetMouseState(int *x, int *y, int *down)
{
    int winX, winY;
    Uint32 buttons = SDL_GetMouseState(&winX, &winY);
    int lx, ly;
    WindowToLogical(winX, winY, &lx, &ly);
    if (x) *x = lx;
    if (y) *y = ly;
    if (down) *down = (buttons & SDL_BUTTON_LMASK) ? 1 : 0;
}

int Platform_GetMouseClick(int *x, int *y)
{
    if (sMouseClicked) {
        if (x) *x = sMouseClickX;
        if (y) *y = sMouseClickY;
        sMouseClicked = 0;
        return 1;
    }
    return 0;
}

/* ========================================================================= */
/*  Cursor                                                                    */
/* ========================================================================= */

void Platform_ShowCursor(void)
{
    SDL_ShowCursor(SDL_ENABLE);
}

void Platform_HideCursor(void)
{
    SDL_ShowCursor(SDL_DISABLE);
}

/* ========================================================================= */
/*  Misc                                                                      */
/* ========================================================================= */

int Platform_ContinuePress(void)
{
    /*
     * Replaces the original ContinuePress(), which checked if any of the
     * "action" buttons were held on a gamepad (kForward, kKickdown, kFire,
     * kMissile). Used to advance past "press any button to continue" screens.
     */
    if (Platform_GetElement(kForward))  return 1;
    if (Platform_GetElement(kKickdown)) return 1;
    if (Platform_GetElement(kFire))     return 1;
    if (Platform_GetElement(kMissile))  return 1;

    /* Also accept Enter / Return from keyboard as a "continue" key. */
    const Uint8 *kbState = SDL_GetKeyboardState(NULL);
    if (kbState[SDL_SCANCODE_RETURN] || kbState[SDL_SCANCODE_KP_ENTER])
        return 1;

    return 0;
}
