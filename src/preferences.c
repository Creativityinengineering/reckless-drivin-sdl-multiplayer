#include "compat.h"
#include "preferences.h"
#include "interface.h"
#include "screen.h"
#include "platform.h"
#include "textrender.h"
#include "error.h"
#include "sprites.h"
#include <stdio.h>
#include <string.h>

tPrefs gPrefs;

typedef struct {
    UInt16 version;
    UInt16 volume;
    UInt8 sound, engineSound, hqSound, skidSound;
    UInt8 lineSkip, motionBlur, hiColor;
    UInt8 hidElements[kNumElements];
    UInt8 unused[11];
    tScoreRecord high[kNumHighScoreEntrys];
    float lapRecords[10];
    Str255 name, code;
    UInt8 keyCodes[kNumElements];
    Str255 lastName;
} tPrefsV5;

static void SetLocalPlayerDefaults(void)
{
    static const UInt8 defaults[kLocalPlayerCount][kLocalBindableCount] = {
        {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
         SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_Z, SDL_SCANCODE_X},
        {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
         SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RCTRL, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2},
        {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
         SDL_SCANCODE_TAB, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_U, SDL_SCANCODE_O},
        {SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6,
         SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_9}
    };
    static const char *names[kLocalPlayerCount] = {
        "Driver 1", "Driver 2", "Driver 3", "Driver 4"
    };
    int player;
    gPrefs.localPlayerCount = 2;
    for (player = 0; player < kLocalPlayerCount; ++player) {
        int length = (int)strlen(names[player]);
        memcpy(gPrefs.localKeyCodes[player], defaults[player], kLocalBindableCount);
        gPrefs.playerNames[player][0] = (UInt8)length;
        memcpy(gPrefs.playerNames[player] + 1, names[player], (size_t)length);
    }
}

static const char *GetPrefsPath(void)
{
    static char path[512];
    char* prefDir = SDL_GetPrefPath("DarrCoh", "RecklessDrivin");
    if (prefDir) {
        snprintf(path, sizeof(path), "%sPreferences", prefDir);
        SDL_free(prefDir);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
        if (!home) home = getenv("APPDATA");
#endif
        if (!home) home = ".";
        snprintf(path, sizeof(path), "%s/.recklessdrivin_prefs", home);
    }

    return path;
}

static void SetDefaults(void)
{
    memset(&gPrefs, 0, sizeof(tPrefs));
    gPrefs.version = kPrefsVersion;
    gPrefs.volume = 200;
    gPrefs.sound = 1;
    gPrefs.engineSound = 1;
    gPrefs.skidSound = 1;
    gPrefs.hqSound = 0;
    gPrefs.hiColor = 1;     /* we only support 16-bit */
    gPrefs.lineSkip = 0;
    gPrefs.motionBlur = 0;
    /* AI-upscaled graphics by default: they need HQ 4K mode (fullscreen at
       twice the resolution). Turn AI Rendering off for the original look. */
    gPrefs.unused[5] = 1;
    gPrefs.unused[6] = 1;
    gPrefs.unused[7] = 1;

    /* Default key codes (SDL scancodes stored directly) */
    gPrefs.keyCodes[kForward]  = SDL_SCANCODE_UP;
    gPrefs.keyCodes[kBackward] = SDL_SCANCODE_DOWN;
    gPrefs.keyCodes[kLeft]     = SDL_SCANCODE_LEFT;
    gPrefs.keyCodes[kRight]    = SDL_SCANCODE_RIGHT;
    gPrefs.keyCodes[kKickdown] = SDL_SCANCODE_LSHIFT;
    gPrefs.keyCodes[kBrake]    = SDL_SCANCODE_SPACE;
    gPrefs.keyCodes[kFire]     = SDL_SCANCODE_Z;
    gPrefs.keyCodes[kMissile]  = SDL_SCANCODE_X;
    SetLocalPlayerDefaults();

    /* Lap records all set to 999.0 */
    {
        int i;
        for (i = 0; i < 10; i++)
            gPrefs.lapRecords[i] = 999.0f;
    }

    /* Registration: Name="Free", Code="B3FB09B1EB" (Pascal strings) */
    gPrefs.name[0] = 4;
    gPrefs.name[1] = 'F';
    gPrefs.name[2] = 'r';
    gPrefs.name[3] = 'e';
    gPrefs.name[4] = 'e';

    gPrefs.code[0] = 10;
    gPrefs.code[1] = 'B';
    gPrefs.code[2] = '3';
    gPrefs.code[3] = 'F';
    gPrefs.code[4] = 'B';
    gPrefs.code[5] = '0';
    gPrefs.code[6] = '9';
    gPrefs.code[7] = 'B';
    gPrefs.code[8] = '1';
    gPrefs.code[9] = 'E';
    gPrefs.code[10] = 'B';
}

void LoadPrefs(void)
{
    FILE *f;
    const char *path = GetPrefsPath();

    f = fopen(path, "rb");
    if (f)
    {
        size_t bytesRead = fread(&gPrefs, 1, sizeof(tPrefs), f);
        fclose(f);
        if (bytesRead == sizeof(tPrefs) && gPrefs.version == kPrefsVersion) {
            if (gPrefs.unused[5] > 1)
                gPrefs.unused[5] = 0;
            if (gPrefs.unused[6] > 1)
                gPrefs.unused[6] = 0;
            if (gPrefs.unused[7] > 1)
                gPrefs.unused[7] = 0;
            return; /* loaded successfully */
        }
        if (bytesRead == sizeof(tPrefsV5) && gPrefs.version == 5) {
            SetLocalPlayerDefaults();
            gPrefs.version = kPrefsVersion;
            WritePrefs(0);
            return;
        }
    }

    /* File doesn't exist or is wrong size/version - use defaults */
    SetDefaults();
    WritePrefs(0);
}

/* gPrefs.unused[9]: 0 = extended mode (more traffic that overtakes, cop
   swarm and traffic jam crates in multiplayer), 1 = the original game. */
int Prefs_IsExtendedMode(void)
{
    return gPrefs.unused[9] != 1;
}

/* gPrefs.unused[10] bit 0: LAN view, 0 = split screen with every driver,
   1 = own screen (only this computer's driver). */
int Prefs_IsLanOwnScreen(void)
{
    return gPrefs.unused[10] & 1;
}

void Prefs_ToggleLanScreen(void)
{
    gPrefs.unused[10] ^= 1;
    WritePrefs(0);
}

void WritePrefs(int reset)
{
    FILE *f;
    const char *path = GetPrefsPath();

    if (reset)
        SetDefaults();

    f = fopen(path, "wb");
    if (f)
    {
        fwrite(&gPrefs, 1, sizeof(tPrefs), f);
        fclose(f);
    }
}

/* ======================================================================== */
/* Preferences UI                                                            */
/* ======================================================================== */

extern void ConfigureInput(void);
static void ConfigurePlayerNames(void);

enum {
    kPrefSound = 0,
    kPrefEngineSound,
    kPrefSkidSound,
    kPrefHQSound,
    kPrefVolume,
    kPrefMotionBlur,
    kPrefHighResScale,
    kPrefHQ4K,
    kPrefAIDetail,
    kPrefGameMode,
    kPrefLanScreen,
    kPrefLocalPlayers,
    kPrefPlayerNames,
    kPrefControls,
    kPrefDone,
    kPrefCount
};

static const char *kPrefLabels[kPrefCount] = {
    "Sound",
    "Engine Sound",
    "Skid Sound",
    "HQ Sound",
    "Volume",
    "Motion Blur",
    "2X Upscaling",
    "HQ 4K Mode",
    "AI Rendering",
    "Game Mode",
    "LAN View",
    "Local Players",
    "Player Names",
    "Player Controls",
    "Done"
};

void Preferences(void)
{
    int rowBytes;
    tPixel *fb = Platform_GetFramebuffer(&rowBytes);
    int fbStride = rowBytes/4;
    int selected = 0;
    int done = 0;

    /* Work on a copy of settings so we can cancel */
    UInt8 snd        = gPrefs.sound;
    UInt8 engSnd     = gPrefs.engineSound;
    UInt8 skidSnd    = gPrefs.skidSound;
    UInt8 hqSnd      = gPrefs.hqSound;
    UInt16 volume    = gPrefs.volume;
    UInt8 motionBlur = gPrefs.motionBlur;
    if (gPrefs.unused[5] > 1)
        gPrefs.unused[5] = 0;
    if (gPrefs.unused[6] > 1)
        gPrefs.unused[6] = 0;
    if (gPrefs.unused[7] > 1)
        gPrefs.unused[7] = 0;

    Platform_ShowCursor();
    SaveFlushEvents();

    while (!done) {
        Platform_PollEvents();
        if (Platform_ShouldQuit()) { done = 1; break; }

        /* --- Draw the preferences screen --- */
        TR_FillRect(fb, fbStride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_DKGRAY);

        /* Title */
        TR_DrawStringCentered(fb, fbStride, SCREEN_WIDTH / 2, 50, "PREFERENCES", COL_WHITE, 3);

        /* Menu items */
        int baseY = 76;
        int itemH = 24;
        int labelX = 140;
        int valueX = 400;

        for (int i = 0; i < kPrefCount; i++) {
            int y = baseY + i * itemH;
            UInt16 labelCol = (i == selected) ? COL_YELLOW : COL_WHITE;

            /* Highlight bar for selected item */
            if (i == selected)
                TR_FillRect(fb, fbStride, 120, y - 4, 400, 28, COL_GRAY);

            if (i == kPrefDone) {
                /* Center the "Done" label */
                TR_DrawStringCentered(fb, fbStride, SCREEN_WIDTH / 2, y, "[ Done ]", labelCol, 2);
            } else if (i == kPrefControls || i == kPrefPlayerNames) {
                TR_DrawString(fb, fbStride, labelX, y, kPrefLabels[i], labelCol, 2);
                TR_DrawString(fb, fbStride, valueX, y,
                              i == kPrefControls ? "Configure..." : "Edit...",
                              (i == selected) ? COL_GREEN : COL_GRAY, 2);
            } else if (i == kPrefLocalPlayers) {
                char playerCount[16];
                TR_DrawString(fb, fbStride, labelX, y, kPrefLabels[i], labelCol, 2);
                snprintf(playerCount, sizeof(playerCount), "%u", gPrefs.localPlayerCount);
                TR_DrawString(fb, fbStride, valueX, y, playerCount,
                              (i == selected) ? COL_GREEN : COL_WHITE, 2);
            } else {
                TR_DrawString(fb, fbStride, labelX, y, kPrefLabels[i], labelCol, 2);

                /* Draw value */
                const char *val = NULL;
                switch (i) {
                    case kPrefSound:       val = snd ? "ON" : "OFF"; break;
                    case kPrefEngineSound: val = engSnd ? "ON" : "OFF"; break;
                    case kPrefSkidSound:   val = skidSnd ? "ON" : "OFF"; break;
                    case kPrefHQSound:     val = hqSnd ? "ON" : "OFF"; break;
                    case kPrefMotionBlur:  val = motionBlur ? "ON" : "OFF"; break;
                    case kPrefHighResScale:
                        val = gPrefs.unused[5] ? "ON 1280x960" : "OFF";
                        break;
                    case kPrefHQ4K:
                        val = gPrefs.unused[7] ? "ON FULLSCREEN" : "OFF";
                        break;
                    case kPrefGameMode:
                        val = Prefs_IsExtendedMode() ? "EXTENDED" : "ORIGINAL";
                        break;
                    case kPrefLanScreen:
                        val = Prefs_IsLanOwnScreen() ? "OWN SCREEN" : "SPLIT SCREEN";
                        break;
                    case kPrefAIDetail:
                        val = !gPrefs.hiColor || !Sprites_HasAIDetailAssets() ? "NOT READY" :
                            (gPrefs.unused[6] ? "ON" : "OFF");
                        break;
                    case kPrefVolume: {
                        /* Volume bar */
                        int barX = valueX;
                        int barW = 120;
                        int barH = 16;
                        int filled = (volume * barW) / 256;
                        TR_FillRect(fb, fbStride, barX, y + 4, barW, barH, COL_BAR_BG);
                        TR_FillRect(fb, fbStride, barX, y + 4, filled, barH, COL_BAR_FG);
                        /* Numeric value */
                        char numBuf[8];
                        snprintf(numBuf, sizeof(numBuf), "%d", volume);
                        TR_DrawString(fb, fbStride, barX + barW + 16, y, numBuf, labelCol, 2);
                        break;
                    }
                }
                if (val)
                    TR_DrawString(fb, fbStride, valueX, y, val,
                                  (i == selected) ? COL_GREEN : COL_WHITE, 2);
            }
        }

        /* Instructions */
        TR_DrawString(fb, fbStride, 130, 405,
            "AI Rendering OFF uses original game graphics", COL_GRAY, 1);
        TR_DrawString(fb, fbStride, 130, 420,
            "HQ render resolution applies after restart", COL_GRAY, 1);
        TR_DrawString(fb, fbStride, 130, 435,
            "Up/Down: Select  Left/Right: Change", COL_GRAY, 1);

        Platform_Blit2Screen();

        /* --- Handle input ---
         * Navigation accepts OS auto-repeat; activation keys need a fresh
         * press so keys held across screen transitions cannot re-trigger. */
        if (Platform_WasKeyTyped(SDL_SCANCODE_UP)) {
            selected = (selected + kPrefCount - 1) % kPrefCount;
        }
        if (Platform_WasKeyTyped(SDL_SCANCODE_DOWN)) {
            selected = (selected + 1) % kPrefCount;
        }

        int adjust = 0;
        if (Platform_WasKeyTyped(SDL_SCANCODE_LEFT))  adjust = -1;
        if (Platform_WasKeyTyped(SDL_SCANCODE_RIGHT)) adjust =  1;
        if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
            Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
            Platform_WasKeyPressed(SDL_SCANCODE_SPACE)) {
            if (selected == kPrefDone) { done = 1; break; }
            if (selected == kPrefControls || selected == kPrefPlayerNames) {
                /* Save current settings before entering controls */
                gPrefs.sound = snd;
                gPrefs.engineSound = engSnd;
                gPrefs.skidSound = skidSnd;
                gPrefs.hqSound = hqSnd;
                gPrefs.volume = volume;
                gPrefs.motionBlur = motionBlur;
                if (selected == kPrefControls)
                    ConfigureInput();
                else
                    ConfigurePlayerNames();
                SaveFlushEvents();
                continue;
            }
            adjust = 1;
        }
        if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE)) {
            done = 1; break;
        }

        /* Mouse click */
        {
            int mx, my;
            if (Platform_GetMouseClick(&mx, &my)) {
                /* Check if click is on a menu item */
                for (int i = 0; i < kPrefCount; i++) {
                    int iy = baseY + i * itemH - 4;
                    if (mx >= 120 && mx < 520 && my >= iy && my < iy + 28) {
                        selected = i;
                        if (i == kPrefDone) { done = 1; break; }
                        if (i == kPrefControls || i == kPrefPlayerNames) {
                            gPrefs.sound = snd;
                            gPrefs.engineSound = engSnd;
                            gPrefs.skidSound = skidSnd;
                            gPrefs.hqSound = hqSnd;
                            gPrefs.volume = volume;
                            gPrefs.motionBlur = motionBlur;
                            if (i == kPrefControls)
                                ConfigureInput();
                            else
                                ConfigurePlayerNames();
                            SaveFlushEvents();
                            continue;
                        }
                        adjust = 1;
                        break;
                    }
                }
            }
        }

        if (adjust && !done) {
            switch (selected) {
                case kPrefSound:       snd = !snd; break;
                case kPrefEngineSound: engSnd = !engSnd; break;
                case kPrefSkidSound:   skidSnd = !skidSnd; break;
                case kPrefHQSound:     hqSnd = !hqSnd; break;
                case kPrefMotionBlur:  motionBlur = !motionBlur; break;
                case kPrefHighResScale:
                    gPrefs.unused[5] = gPrefs.unused[5] ? 0 : 1;
                    Platform_SetUpscaleMode(gPrefs.unused[7] ? kPlatformOutput4K :
                        (gPrefs.unused[5] ? kPlatformOutput2X :
                            kPlatformOutputOriginal));
                    break;
                case kPrefHQ4K:
                    gPrefs.unused[7] = gPrefs.unused[7] ? 0 : 1;
                    if (gPrefs.unused[7])
                        gPrefs.unused[5] = 1;
                    Platform_SetUpscaleMode(gPrefs.unused[7] ? kPlatformOutput4K :
                        (gPrefs.unused[5] ? kPlatformOutput2X :
                            kPlatformOutputOriginal));
                    break;
                case kPrefGameMode:
                    gPrefs.unused[9] = gPrefs.unused[9] == 1 ? 0 : 1;
                    break;
                case kPrefLanScreen:
                    gPrefs.unused[10] ^= 1;
                    break;
                case kPrefAIDetail:
                    if (gPrefs.hiColor && Sprites_HasAIDetailAssets()) {
                        gPrefs.unused[6] = gPrefs.unused[6] ? 0 : 1;
                        if (gPrefs.unused[6]) {
                            gPrefs.unused[5] = 1;
                            gPrefs.unused[7] = 1;
                        }
                        UnloadSprites();
                        LoadSprites();
                        Platform_SetUpscaleMode(gPrefs.unused[7] ? kPlatformOutput4K :
                            (gPrefs.unused[5] ? kPlatformOutput2X :
                                kPlatformOutputOriginal));
                    }
                    break;
                case kPrefLocalPlayers: {
                    int count = (int)gPrefs.localPlayerCount + adjust;
                    if (count < 1) count = kLocalPlayerCount;
                    if (count > kLocalPlayerCount) count = 1;
                    gPrefs.localPlayerCount = (UInt8)count;
                    break;
                }
                case kPrefVolume: {
                    int v = (int)volume + adjust * 10;
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                    volume = (UInt16)v;
                    break;
                }
            }
        }

        SDL_Delay(16);
    }

    /* Save settings */
    gPrefs.sound = snd;
    gPrefs.engineSound = engSnd;
    gPrefs.skidSound = skidSnd;
    gPrefs.hqSound = hqSnd;
    gPrefs.volume = volume;
    gPrefs.motionBlur = motionBlur;
    Platform_SetUpscaleMode(gPrefs.unused[7] ? kPlatformOutput4K :
        (gPrefs.unused[5] ? kPlatformOutput2X : kPlatformOutputOriginal));
    WritePrefs(0);

    /* Return to menu */
    ShowPicScreen(1000);
    SaveFlushEvents();
}

static void ConfigurePlayerNames(void)
{
    int rowBytes, done = 0, editing = 0, selected = 0;
    tPixel *fb = Platform_GetFramebuffer(&rowBytes);
    int stride = rowBytes/4;
    char names[kLocalPlayerCount][16];
    int player;
    for (player = 0; player < kLocalPlayerCount; ++player) {
        int length = gPrefs.playerNames[player][0];
        if (length > 15) length = 15;
        memcpy(names[player], gPrefs.playerNames[player] + 1, (size_t)length);
        names[player][length] = '\0';
    }

    Platform_ShowCursor();
    Platform_FlushInput();
    SDL_StartTextInput();
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                extern int gExit;
                gExit = 1;
                done = 1;
                break;
            }
            if (event.type == SDL_TEXTINPUT && editing) {
                for (int i = 0; event.text.text[i] && strlen(names[selected]) < 15; ++i) {
                    unsigned char c = (unsigned char)event.text.text[i];
                    if (c >= 32 && c <= 126) {
                        size_t length = strlen(names[selected]);
                        names[selected][length] = (char)c;
                        names[selected][length + 1] = '\0';
                    }
                }
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                SDL_Keycode key = event.key.keysym.sym;
                if (editing) {
                    if (key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                        key == SDLK_ESCAPE) {
                        editing = 0;
                        SDL_StopTextInput();
                    } else if (key == SDLK_BACKSPACE) {
                        size_t length = strlen(names[selected]);
                        if (length) names[selected][length - 1] = '\0';
                    }
                } else if (key == SDLK_ESCAPE) {
                    done = 1;
                } else if (key == SDLK_UP) {
                    selected = (selected + kLocalPlayerCount - 1) % kLocalPlayerCount;
                } else if (key == SDLK_DOWN) {
                    selected = (selected + 1) % kLocalPlayerCount;
                } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                           key == SDLK_SPACE) {
                    editing = 1;
                    SDL_StartTextInput();
                }
            }
        }
        if (done) break;
        TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_DKGRAY);
        TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 54,
            "PLAYER NAMES", COL_WHITE, 3);
        for (player = 0; player < kLocalPlayerCount; ++player) {
            char label[24];
            int y = 128 + player * 52;
            UInt16 color = player == selected ? COL_YELLOW : COL_WHITE;
            if (player == selected)
                TR_FillRect(fb, stride, 120, y - 6, 400, 36, COL_GRAY);
            snprintf(label, sizeof(label), "PLAYER %d", player + 1);
            TR_DrawString(fb, stride, 145, y, label, color, 2);
            TR_DrawStringRight(fb, stride, 490, y, names[player],
                player == selected && editing ? COL_GREEN : COL_WHITE, 2);
        }
        TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 370,
            editing ? "TYPE NAME  ENTER: DONE" : "ENTER: EDIT  ESC: BACK",
            COL_GRAY, 2);
        TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 404,
            "NAMES ARE SHOWN DURING THE RACE", COL_GRAY, 1);
        Platform_Blit2Screen();
        SDL_Delay(16);
    }
    if (editing)
        SDL_StopTextInput();
    for (player = 0; player < kLocalPlayerCount; ++player) {
        size_t length = strlen(names[player]);
        gPrefs.playerNames[player][0] = (UInt8)length;
        memcpy(gPrefs.playerNames[player] + 1, names[player], length);
    }
    WritePrefs(0);
    Platform_HideCursor();
    Platform_FlushInput();
}
