/*
 * input.c - Input handling for Reckless Drivin' SDL port
 *
 * Replaces the original InputSprocket / HID Utilities / iShock force-feedback
 * code with calls to the SDL2 platform abstraction layer.
 *
 * The core game logic in Input() is preserved exactly: throttle ramping,
 * steering smoothing, brake handling, gear-switch delay, and event dispatch
 * (fire, missile, pause, abort) all work the same as the original.
 */

#include "compat.h"
#include "input.h"
#include "netgame.h"
#include "platform.h"
#include "objects.h"
#include "screen.h"
#include "gameframe.h"
#include "preferences.h"
#include "textrender.h"

/* ======================================================================== */
/* Constants                                                                 */
/* ======================================================================== */

#define kMinSwitchDelay  15  /* Minimum delay between switching reverse gears (frames) */

/* ======================================================================== */
/* Globals                                                                   */
/* ======================================================================== */

tInputData    gInputData;
int           gFire, gMissile;
unsigned long gSwitchDelayStart;
static int sLocalElementPrevState[kLocalPlayerCount][kNumElements];

/* ======================================================================== */
/* Init / Mode                                                               */
/* ======================================================================== */

void InitInput(void)
{
    Platform_InitInput();
}

void InputMode(int mode)
{
    switch (mode) {
    case kInputRunning:
        Platform_FlushInput();
        break;
    case kInputSuspended:
        break;
    case kInputStopped:
        Platform_ShutdownInput();
        break;
    }
}

/* ======================================================================== */
/* Element / Event queries                                                   */
/* ======================================================================== */

int GetElement(int element)
{
    int player = Network_GetLocalDriverSlot(gPlayerObj);
    if (player >= 0 && element >= 0 && element < kLocalBindableCount) {
        SDL_Scancode key = (SDL_Scancode)gPrefs.localKeyCodes[player][element];
        return Platform_IsKeyDown(key);
    }
    return Platform_GetElement(element);
}

int GetEvent(int *element, int *data)
{
    int player = Network_GetLocalDriverSlot(gPlayerObj);
    if (player >= 0) {
        int current;
        for (int i = 0; i < kNumElements; ++i) {
            current = GetElement(i);
            if (current != sLocalElementPrevState[player][i]) {
                *element = i;
                *data = current;
                sLocalElementPrevState[player][i] = current;
                return 1;
            }
        }
        return 0;
    }
    return Platform_GetEvent(element, data);
}

/* ======================================================================== */
/* Throttle reset helper                                                     */
/* ======================================================================== */

static float ThrottleReset(float throttle)
{
    if (throttle > 0) {
        throttle -= 2 * kFrameDuration;
        if (throttle < 0)
            throttle = 0;
    } else if (throttle < 0) {
        throttle += 2 * kFrameDuration;
        if (throttle > 0)
            throttle = 0;
    }
    return throttle;
}

/* ======================================================================== */
/* Main input processing -- pure game logic, no platform calls               */
/* ======================================================================== */

extern void PauseGame(void);

void Input(tInputData **data)
{
    int playerVelo = VEC2D_DotProduct(gPlayerObj->velo,
                         P2D(sin(gPlayerObj->dir), cos(gPlayerObj->dir)));
    int axState = 0;
    int switchRequest = false;
    int element, eventData;

    gMissile = false;
    gFire = false;

    if (playerVelo)
        gSwitchDelayStart = gFrameCount;

    /* Poll SDL events so keyboard state is fresh */
    Platform_PollEvents();

    /* Handbrake ramp */
    gInputData.handbrake = (GetElement(kBrake)
                            ? gInputData.handbrake + kFrameDuration * 8
                            : 0);
    if (gInputData.handbrake > 1) gInputData.handbrake = 1;

    gInputData.kickdown = false;

    /* Forward / backward axis */
    if (GetElement(kForward))  axState = 1;
    if (GetElement(kBackward)) axState = -1;

    switch (axState) {
    case 1:
        if (!gInputData.reverse) {
            gInputData.throttle += 3 * kFrameDuration;
            if (gInputData.throttle > 1) gInputData.throttle = 1;
            gInputData.brake = 0;
            if (playerVelo < 0) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
        } else {
            gInputData.brake += kFrameDuration * 6;
            if (gInputData.brake > 1) gInputData.brake = 1;
            gInputData.throttle = ThrottleReset(gInputData.throttle);
            if (!playerVelo)
                switchRequest = true;
            if (playerVelo > 0) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
        }
        break;

    case -1:
        if (gInputData.reverse) {
            gInputData.throttle -= 3 * kFrameDuration;
            if (gInputData.throttle < -1) gInputData.throttle = -1;
            gInputData.brake = 0;
            if (playerVelo > 0) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
        } else {
            gInputData.brake += kFrameDuration * 6;
            if (gInputData.brake > 1) gInputData.brake = 1;
            gInputData.throttle = ThrottleReset(gInputData.throttle);
            if (!playerVelo)
                switchRequest = true;
            if (playerVelo < 0) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
        }
        break;

    case 0:
        gInputData.brake = 0;
        gInputData.throttle = ThrottleReset(gInputData.throttle);
        break;
    }

    /* Kickdown */
    if (GetElement(kKickdown) && axState != -1) {
        gInputData.kickdown = true;
        gInputData.throttle = 1;
        if (gInputData.reverse && !gInputData.brake) {
            switchRequest = true;
            gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
        }
    }

    /* Steering axis */
    axState = 0;
    if (GetElement(kRight)) axState = 1;
    if (GetElement(kLeft))  axState = -1;

    switch (axState) {
    case 1:
        gInputData.steering += (gInputData.steering < 0) ? 8 : 3 * kFrameDuration;
        if (gInputData.steering > 1) gInputData.steering = 1;
        break;
    case -1:
        gInputData.steering -= (gInputData.steering > 0) ? 8 : 3 * kFrameDuration;
        if (gInputData.steering < -1) gInputData.steering = -1;
        break;
    case 0:
        if (gInputData.steering > 0) {
            gInputData.steering -= 8 * kFrameDuration;
            if (gInputData.steering < 0)
                gInputData.steering = 0;
        } else {
            gInputData.steering += 8 * kFrameDuration;
            if (gInputData.steering > 0)
                gInputData.steering = 0;
        }
        break;
    }

    /* Process button events (fire, missile, abort, pause) */
    while (GetEvent(&element, &eventData)) {
        switch (element) {
        case kForward:
            if (!Network_IsLocalMultiplayer() &&
                !playerVelo && gInputData.reverse && eventData) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
            break;
        case kBackward:
            if (!Network_IsLocalMultiplayer() &&
                !playerVelo && !gInputData.reverse && eventData) {
                switchRequest = true;
                gSwitchDelayStart = gFrameCount - kMinSwitchDelay;
            }
            break;
        case kMissile:
            gMissile = eventData;
            break;
        case kFire:
            gFire = eventData;
            break;
        case kAbort:
            /* Fire on the press edge only. Acting on the release would abort
             * the level when a held Escape (e.g. used to dismiss the pause
             * screen) is let go after gameplay resumes. */
            if (eventData)
                gEndGame = true;
            break;
        case kPause:
            if (eventData)
                PauseGame();
            break;
        }
    }

    /* Handle gear switch request with delay */
    if (switchRequest && gFrameCount >= gSwitchDelayStart + kMinSwitchDelay)
        gInputData.reverse = !gInputData.reverse;

    *data = &gInputData;
}

/* ======================================================================== */
/* Timing                                                                    */
/* ======================================================================== */

UInt64 GetMSTime(void)
{
    return Platform_GetMicroseconds();
}

/* ======================================================================== */
/* Flush                                                                     */
/* ======================================================================== */

void FlushInput(void)
{
    Platform_FlushInput();
}

/* ======================================================================== */
/* Force feedback stubs (no force feedback on modern systems)                */
/* ======================================================================== */

void FFBJolt(float lMag, float rMag, float duration)
{
    (void)lMag;
    (void)rMag;
    (void)duration;
}

void FFBDirect(float lMag, float rMag)
{
    (void)lMag;
    (void)rMag;
}

void FFBStop(void)
{
    /* no-op */
}

/* ======================================================================== */
/* Continue press                                                            */
/* ======================================================================== */

int ContinuePress(void)
{
    return Platform_ContinuePress();
}

/* ======================================================================== */
/* Configuration stub (called from interface.c)                              */
/* ======================================================================== */

void ConfigureInput(void)
{
    static const char *kElementNames[] = {
        "Forward", "Backward", "Left", "Right",
        "Kickdown", "Brake", "Mine", "Missile"
    };
    enum { kNumBindable = kLocalBindableCount, kConfigDone = kNumBindable };
    int selected = 0, profile = 0, done = 0, waiting = 0, rowBytes;
    tPixel *fb = Platform_GetFramebuffer(&rowBytes);
    int fbStride = rowBytes/4;

    Platform_ShowCursor();
    Platform_FlushInput();

    while (!done) {
        char title[32];
        Platform_PollEvents();
        if (Platform_ShouldQuit()) { done = 1; break; }

        if (profile == 0)
            snprintf(title, sizeof(title), "CONTROLS - SOLO");
        else
            snprintf(title, sizeof(title), "CONTROLS - PLAYER %d", profile);
        TR_FillRect(fb, fbStride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_DKGRAY);
        TR_DrawStringCentered(fb, fbStride, SCREEN_WIDTH / 2, 42, title, COL_WHITE, 2);
        TR_DrawStringCentered(fb, fbStride, SCREEN_WIDTH / 2, 72,
            "LEFT/RIGHT: SOLO OR MULTIPLAYER DRIVER", COL_CYAN, 1);

        {
        int baseY = 105;
        int itemH = 34;
        for (int i = 0; i <= kConfigDone; i++) {
            int y = baseY + i * itemH;
            UInt16 labelCol = (i == selected) ? COL_YELLOW : COL_WHITE;

            if (i == selected)
                TR_FillRect(fb, fbStride, 120, y - 4, 400, 26, COL_GRAY);

            if (i == kConfigDone) {
                TR_DrawStringCentered(fb, fbStride, SCREEN_WIDTH / 2, y, "[ Done ]", labelCol, 2);
            } else {
                TR_DrawString(fb, fbStride, 140, y, kElementNames[i], labelCol, 2);

                if (waiting && i == selected) {
                    TR_DrawString(fb, fbStride, 370, y, "Press a key...", COL_RED, 2);
                } else {
                    SDL_Scancode key = profile == 0 ?
                        Platform_GetElementKey(i) :
                        (SDL_Scancode)gPrefs.localKeyCodes[profile - 1][i];
                    const char *keyName = SDL_GetScancodeName(key);
                    if (!keyName || keyName[0] == '\0') keyName = "???";
                    TR_DrawString(fb, fbStride, 370, y,  keyName,
                                  (i == selected) ? COL_GREEN : COL_WHITE, 2);
                }
            }
        }

        }
        TR_DrawString(fb, fbStride, 130, 420, "Enter: Rebind   Esc: Back", COL_GRAY, 2);

        Platform_Blit2Screen();

        if (waiting) {
            SDL_Scancode sc = Platform_WaitForKey();
            if (sc != SDL_SCANCODE_UNKNOWN && sc < 256) {
                if (profile == 0) {
                    Platform_SetElementKey(selected, sc);
                    gPrefs.keyCodes[selected] = (UInt8)sc;
                } else {
                    gPrefs.localKeyCodes[profile - 1][selected] = (UInt8)sc;
                }
            }
            waiting = 0;
            Platform_FlushInput();
            continue;
        }

        if (Platform_WasKeyTyped(SDL_SCANCODE_UP)) {
            selected = (selected + kConfigDone) % (kConfigDone + 1);
        }
        if (Platform_WasKeyTyped(SDL_SCANCODE_DOWN)) {
            selected = (selected + 1) % (kConfigDone + 1);
        }
        if (Platform_WasKeyTyped(SDL_SCANCODE_LEFT)) {
            profile = (profile + kLocalPlayerCount) % (kLocalPlayerCount + 1);
        }
        if (Platform_WasKeyTyped(SDL_SCANCODE_RIGHT)) {
            profile = (profile + 1) % (kLocalPlayerCount + 1);
        }
        if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
            Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
            Platform_WasKeyPressed(SDL_SCANCODE_SPACE)) {
            if (selected == kConfigDone) {
                done = 1;
            } else {
                waiting = 1;
            }
        }
        if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE)) {
            done = 1;
        }

        {
            int mx, my;
            if (Platform_GetMouseClick(&mx, &my)) {
                int baseY = 105;
                int itemH = 34;
                for (int i = 0; i <= kConfigDone; i++) {
                    int iy = baseY + i * itemH - 4;
                    if (mx >= 120 && mx < 520 && my >= iy && my < iy + 26) {
                        selected = i;
                        if (i == kConfigDone) { done = 1; break; }
                        waiting = 1;
                        break;
                    }
                }
            }
        }

        SDL_Delay(16);
    }

    for (int i = 0; i < kNumBindable; i++)
        gPrefs.keyCodes[i] = (UInt8)Platform_GetElementKey(i);
    WritePrefs(0);

    Platform_FlushInput();
}
