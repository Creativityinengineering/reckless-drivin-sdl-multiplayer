#ifndef __PLATFORM_H
#define __PLATFORM_H

/*
 * platform.h - SDL2 platform abstraction for Reckless Drivin'
 * Replaces DrawSprocket, InputSprocket, Sound Manager, etc.
 */

#include "compat.h"
#include "pixel.h"
#include <SDL.h>

/* Screen */
#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 480
#define FRAMEBUFFER_GUARD_ROWS 64
#define PLATFORM_MAX_RENDER_SCALE 4
#define PLATFORM_HQ_RENDER_SCALE 2

void Platform_InitScreen(void);
void Platform_ShutdownScreen(void);
void Platform_Blit2Screen(void);
void Platform_BeginMultiView(int viewCount);
void Platform_ScaleToFitWindow(void);
int Platform_GetRenderScale(void);
int Platform_GetRenderWidth(void);
int Platform_GetRenderHeight(void);
enum {
    kPlatformOutputOriginal = 0,
    kPlatformOutput2X,
    kPlatformOutput4K
};
void Platform_SetUpscaleMode(int outputMode);
void Platform_SetAIDetailSprite(int id, int width, int height,
                                const tPixel *pixels);
void Platform_ClearAIDetailSprites(void);
void Platform_QueueAIDetailSprite(int id, float centerX, float centerY,
                                  float direction, float zoom);
void Platform_ToggleFullscreen(void);
void Platform_FadeScreen(int fade);
void Platform_ScreenClear(void);

/* Returns the 32-bit framebuffer (see pixel.h) and its row stride in bytes */
tPixel* Platform_GetFramebuffer(int *rowBytes);

/* Input */
void Platform_InitInput(void);
void Platform_ShutdownInput(void);
void Platform_PollEvents(void);
int Platform_IsKeyDown(int scancode);
/* One-shot key events, consumed on read (like Platform_GetMouseClick).
 * WasKeyPressed: physical press only; a key held across a screen
 * transition cannot re-trigger until released and pressed again.
 * WasKeyTyped: press or OS auto-repeat; for hold-to-scroll navigation. */
int Platform_WasKeyPressed(int scancode);
int Platform_WasKeyTyped(int scancode);
int Platform_GetElement(int element);
int Platform_GetEvent(int *element, int *data);
void Platform_FlushInput(void);
int Platform_ShouldQuit(void);

/* Map SDL scancodes to our element IDs */
/* Default key mapping (matching original defaults) */
enum {
    kDefaultKeyForward   = SDL_SCANCODE_UP,
    kDefaultKeyBackward  = SDL_SCANCODE_DOWN,
    kDefaultKeyLeft      = SDL_SCANCODE_LEFT,
    kDefaultKeyRight     = SDL_SCANCODE_RIGHT,
    kDefaultKeyKickdown  = SDL_SCANCODE_LSHIFT,
    kDefaultKeyBrake     = SDL_SCANCODE_SPACE,
    kDefaultKeyFire      = SDL_SCANCODE_Z,
    kDefaultKeyMissile   = SDL_SCANCODE_X,
    kDefaultKeyAbort     = SDL_SCANCODE_ESCAPE,
    kDefaultKeyPause     = SDL_SCANCODE_P
};

/* Audio */
#define AUDIO_MIX_RATE 44100

void Platform_InitAudio(void);
void Platform_ShutdownAudio(void);
void Platform_PlaySound(int channelId, const void *sampleData, int sampleLen,
                        float volume, float pan, float pitch,
                        int bitsPerSample);
void Platform_StopChannel(int channelId);
int  Platform_IsChannelActive(int channelId);
void Platform_SetChannelVolume(int channelId, float left, float right);
void Platform_SetChannelPitch(int channelId, float pitch);

/* Engine and skid loops: one pair per driver playing on this computer (up
   to four in split screen); driver n uses kAudioChannelEngine + 2 * n and
   kAudioChannelSkid + 2 * n. */
#define kAudioCarChannels 4
enum {
    kAudioChannel0 = 0,
    kAudioChannelEngine = 16,
    kAudioChannelSkid,
    kNumAudioChannels = kAudioChannelEngine + 2 * kAudioCarChannels
};

#define kNumSFXChannels 16  /* SFX channels: 0 through 15 */

/* Timing */
UInt64 Platform_GetMicroseconds(void);
UInt64 Platform_TakePresentMicroseconds(void);

/* Cursor */
void Platform_ShowCursor(void);
void Platform_HideCursor(void);

/* Mouse */
void Platform_GetMouseState(int *x, int *y, int *down);
int  Platform_GetMouseClick(int *x, int *y);

/* Key mapping */
void Platform_SetElementKey(int element, SDL_Scancode key);
SDL_Scancode Platform_GetElementKey(int element);
SDL_Scancode Platform_WaitForKey(void);  /* blocks until key pressed; returns 0 if cancelled */
void Platform_LoadKeyBindings(const UInt8 *keyCodes); /* load saved bindings from prefs */

/* Misc */
int Platform_ContinuePress(void);

#endif /* __PLATFORM_H */
