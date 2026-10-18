#include "compat.h"
#include "error.h"
#include "screen.h"
#include "input.h"
#include "gameinitexit.h"
#include "preferences.h"
#include "high.h"
#include "gamesounds.h"
#include "lzrw.h"
#include "resources.h"
#include "register.h"
#include "platform.h"
#include "netgame.h"
#include "levelgen.h"
#include "textrender.h"
#include "hqassets.h"
#include <stdio.h>
#include <string.h>

enum{
	kNoButton=-1,
	kStartGameButton,
	kPrefsButton,
	kScoreButton,
	kHelpButton,
	kQuitButton,
	kRegisterButton,
	kMultiplayerButton,
	kAboutButton
};

/* These globals are defined in main.c; we just reference them here */
extern int gExit;
extern short gLevelResFile,gAppResFile;
unsigned char gLevelFileName[64];
static int gInterfaceInited=false;

/* Menu image buffers for hover/press visual feedback */
static tPixel sMenuNormal[SCREEN_WIDTH * PLATFORM_MAX_RENDER_SCALE *
	SCREEN_HEIGHT * PLATFORM_MAX_RENDER_SCALE];
static tPixel sMenuHover[SCREEN_WIDTH * PLATFORM_MAX_RENDER_SCALE *
	SCREEN_HEIGHT * PLATFORM_MAX_RENDER_SCALE];
static tPixel sMenuPressed[SCREEN_WIDTH * PLATFORM_MAX_RENDER_SCALE *
	SCREEN_HEIGHT * PLATFORM_MAX_RENDER_SCALE];
static int sMenuImagesLoaded = 0;
static int sHoverButton = -1; /* which button the mouse is currently over */
extern void ConfigureInput(void);

/* Button hit rectangles from the original 'Recs' resource id=1000.
 * Each entry is {top, left, bottom, right} in 640x480 coordinates.
 * Order matches the button enum: Start, Prefs, Scores, Help, Quit, Register,
 * Multiplayer. */
typedef struct { int top, left, bottom, right; } ButtonRect;
static const ButtonRect kButtonRects[] = {
	{105, 389, 202, 588},  /* kStartGameButton */
	{125, 114, 193, 250},  /* kPrefsButton     */
	{267,  69, 334, 199},  /* kScoreButton     */
	{284, 433, 352, 563},  /* kHelpButton      */
	{392, 470, 458, 601},  /* kQuitButton      */
	{231, 257, 297, 389},  /* kRegisterButton  */
	{372, 270, 450, 422},  /* kMultiplayerButton */
};
#define kNumButtons (sizeof(kButtonRects) / sizeof(kButtonRects[0]))
#define kMultiplayerPort 33777

static int HitTestButtons(int x, int y)
{
	for (int i = 0; i < (int)kNumButtons; i++) {
		if (x >= kButtonRects[i].left && x < kButtonRects[i].right &&
		    y >= kButtonRects[i].top  && y < kButtonRects[i].bottom)
			return i;
	}
	return kNoButton;
}

/* Forward declaration — ShowPicScreen is defined later in this file */
void ShowPicScreen(int id);
static void RestoreMenuScreen(void);

/* ======================================================================== */
/* Menu image helpers for hover/press states                                 */
/* ======================================================================== */

/* Load a PPic into a render-sized buffer by rendering to the framebuffer
 * and copying. Returns 1 on success. */
static int LoadPPicToBuffer(int ppicId, tPixel *buf)
{
	int rowBytes;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	ShowPicScreen(ppicId);
	memcpy(buf, fb, (size_t)(rowBytes/4) * Platform_GetRenderHeight() *
		sizeof(tPixel));
	return 1;
}

/* Copy just one button's rectangle from a source buffer to the framebuffer */
static void BlitButtonRect(int button, tPixel *source)
{
	if (button < 0 || button >= (int)kNumButtons) return;
	int rowBytes;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	int fbStride = rowBytes/4;
	const ButtonRect *r = &kButtonRects[button];
	int scale = Platform_GetRenderScale();
	for (int y = r->top * scale; y < r->bottom * scale &&
		y < Platform_GetRenderHeight(); y++) {
		for (int x = r->left * scale; x < r->right * scale &&
			x < Platform_GetRenderWidth(); x++) {
			fb[y * fbStride + x] = source[y * fbStride + x];
		}
	}
}

/* Map a keyboard shortcut to the button index it corresponds to.
 * Returns kNoButton if no match. Presses are consumed on read, so a key
 * held down while a submenu closes cannot re-trigger its button. */
static int KeyToButton(void)
{
	if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
	    Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
	    Platform_WasKeyPressed(SDL_SCANCODE_S) ||
	    Platform_WasKeyPressed(SDL_SCANCODE_N))
		return kStartGameButton;
	if (Platform_WasKeyPressed(SDL_SCANCODE_P))
		return kPrefsButton;
	if (Platform_WasKeyPressed(SDL_SCANCODE_C) ||
	    Platform_WasKeyPressed(SDL_SCANCODE_O))
		return kScoreButton;
	if (Platform_WasKeyPressed(SDL_SCANCODE_H) ||
	    Platform_WasKeyPressed(SDL_SCANCODE_SLASH))
		return kHelpButton;
	if (Platform_WasKeyPressed(SDL_SCANCODE_Q))
		return kQuitButton;
	if (Platform_WasKeyPressed(SDL_SCANCODE_M))
		return kMultiplayerButton;
	return kNoButton;
}

/* ---- Multiplayer licence plate in the main menu ----
   A Hawaii plate, to go with the other states' plates: white with the
   rainbow arch, embossed blue letters, chrome frame and screws. Drawn at
   full render resolution; hover gives the frame a golden glow. */
#define kPlateX 270.0f
#define kPlateY 372.0f
#define kPlateW 150.0f
#define kPlateH 76.0f

/* Coverage (0..1) of a rounded rectangle at a pixel, 1 pixel soft edge. */
static float RoundedRectCoverage(float px, float py, float x0, float y0,
	float x1, float y1, float radius, float soft)
{
	float cx = px < x0 + radius ? x0 + radius : (px > x1 - radius ? x1 - radius : px);
	float cy = py < y0 + radius ? y0 + radius : (py > y1 - radius ? y1 - radius : py);
	float dx = px - cx, dy = py - cy;
	float distance = sqrtf(dx * dx + dy * dy) - radius;
	float coverage = 0.5f - distance / soft;
	return coverage < 0 ? 0 : (coverage > 1 ? 1 : coverage);
}

static tPixel MixRGB(int r0, int g0, int b0, int r1, int g1, int b1, float t)
{
	return PixelRGB((int)(r0 + (r1 - r0) * t), (int)(g0 + (g1 - g0) * t),
		(int)(b0 + (b1 - b0) * t));
}

/* Grime for the plate: deterministic noise, so every menu draw looks the
   same. */
static float PlateHash(int x, int y)
{
	UInt32 h = (UInt32)x * 374761393u + (UInt32)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float PlateNoise(float x, float y)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx = x - ix, fy = y - iy;
	float a = PlateHash(ix, iy), b = PlateHash(ix + 1, iy);
	float c = PlateHash(ix, iy + 1), d = PlateHash(ix + 1, iy + 1);
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static tPixel ShiftPixel(tPixel p, int delta)
{
	int r = (int)((p >> 16) & 0xFF) + delta, g = (int)((p >> 8) & 0xFF) + delta,
		b = (int)(p & 0xFF) + delta;
	r = r < 0 ? 0 : (r > 255 ? 255 : r);
	g = g < 0 ? 0 : (g > 255 ? 255 : g);
	b = b < 0 ? 0 : (b > 255 ? 255 : b);
	return PixelRGB(r, g, b);
}

/* Road dirt, grain, scratches and darker edges over the finished plate,
   letters included: it has been on a car for a while. */
static void WeatherPlate(tPixel *pixels, int stride, float x0, float y0,
	float x1, float y1, float s)
{
	static const float kScratches[4][4] = {
		{0.18f, 0.30f, 0.34f, 0.22f}, {0.58f, 0.70f, 0.80f, 0.56f},
		{0.70f, 0.20f, 0.86f, 0.34f}, {0.10f, 0.78f, 0.24f, 0.86f}
	};
	int width = Platform_GetRenderWidth(), height = Platform_GetRenderHeight();
	int px, py, i;
	for (py = (int)y0; py < (int)y1; py++)
		for (px = (int)x0; px < (int)x1; px++) {
			float fx = px + 0.5f, fy = py + 0.5f;
			float plate = RoundedRectCoverage(fx, fy, x0, y0, x1, y1, 7 * s, 1.2f);
			float edge, dirt, amount, u, v;
			tPixel *dst;
			if (plate <= 0 || px < 0 || py < 0 || px >= width || py >= height)
				continue;
			dst = pixels + py * stride + px;
			u = (fx - x0) / (x1 - x0);
			v = (fy - y0) / (y1 - y0);
			/* closeness to the border, 1 at the edge */
			edge = fminf(fminf(fx - x0, x1 - fx), fminf(fy - y0, y1 - fy)) / (12 * s);
			edge = edge > 1 ? 0 : 1 - edge;
			dirt = PlateNoise(fx / (9 * s), fy / (9 * s)) * 0.65f +
				PlateNoise(fx / (3 * s), fy / (3 * s)) * 0.35f;
			amount = (dirt * 0.9f + edge * 0.55f + v * 0.35f - 0.78f) * 1.8f;
			if (amount > 0) {
				if (amount > 1)
					amount = 1;
				*dst = BlendAlpha(PixelRGB(98, 84, 62), *dst, (UInt32)(amount * 120 * plate));
			}
			/* darker towards the corners */
			*dst = ShiftPixel(*dst, (int)(-18 * ((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2));
			/* grain */
			*dst = ShiftPixel(*dst, (int)((PlateHash(px, py) - 0.5f) * 16));
		}
	/* a few fine scratches */
	for (i = 0; i < 4; i++) {
		float ax = x0 + (x1 - x0) * kScratches[i][0], ay = y0 + (y1 - y0) * kScratches[i][1];
		float bx = x0 + (x1 - x0) * kScratches[i][2], by = y0 + (y1 - y0) * kScratches[i][3];
		int steps = (int)(fabsf(bx - ax) + fabsf(by - ay)) + 1, step;
		for (step = 0; step <= steps; step++) {
			float t = (float)step / steps;
			px = (int)(ax + (bx - ax) * t);
			py = (int)(ay + (by - ay) * t + sinf(t * 9 + i) * s);
			if (px >= 0 && py >= 0 && px < width && py < height)
				pixels[py * stride + px] = BlendAlpha(PixelRGB(250, 250, 245),
					pixels[py * stride + px], i & 1 ? 70 : 45);
		}
	}
}

static void DrawLicensePlate(tPixel *pixels, int stride, int hover, int pressed)
{
	static const int kRainbow[6][3] = {
		{196, 82, 76}, {214, 150, 84}, {216, 198, 104},
		{110, 160, 106}, {92, 128, 186}, {130, 100, 160}
	};
	float s = (float)Platform_GetRenderScale();
	float offset = pressed ? 1.0f : 0.0f;
	float x0 = (kPlateX + offset) * s, y0 = (kPlateY + offset) * s;
	float x1 = x0 + kPlateW * s, y1 = y0 + kPlateH * s;
	float inset = 3.5f * s;
	float arcX = (x0 + x1) / 2, arcY = y0 + kPlateH * 1.62f * s;
	int width = Platform_GetRenderWidth(), height = Platform_GetRenderHeight();
	int px, py;
	for (py = (int)y0 - 2; py < (int)(y1 + 8 * s); py++)
		for (px = (int)x0 - 2; px < (int)(x1 + 6 * s); px++) {
			tPixel *dst;
			float fx = px + 0.5f, fy = py + 0.5f;
			float shadow, frame, face;
			if (px < 0 || py < 0 || px >= width || py >= height)
				continue;
			dst = pixels + py * stride + px;
			/* soft drop shadow */
			shadow = RoundedRectCoverage(fx, fy, x0 + 2 * s, y0 + 3 * s,
				x1 + 2 * s, y1 + 3 * s, 8 * s, 3 * s);
			if (shadow > 0)
				*dst = BlendAlpha(PixelRGB(0, 0, 0), *dst, (UInt32)(shadow * 70));
			/* dark frame like the other plates' (golden on hover) */
			frame = RoundedRectCoverage(fx, fy, x0, y0, x1, y1, 7 * s, 1.2f);
			if (frame > 0) {
				float t = (fy - y0) / (y1 - y0);
				tPixel color = hover ?
					MixRGB(226, 190, 96, 150, 104, 30, t) :
					MixRGB(78, 76, 80, 34, 32, 36, t);
				/* a faint rim on top */
				if (fy - y0 < 1.2f * s)
					color = BlendAlpha(PixelRGB(200, 200, 200), color, 70);
				*dst = BlendAlpha(color, *dst, (UInt32)(frame * 255));
			}
			/* the plate itself */
			face = RoundedRectCoverage(fx, fy, x0 + inset, y0 + inset,
				x1 - inset, y1 - inset, 5 * s, 1.2f);
			if (face > 0) {
				float t = (fy - y0 - inset) / (y1 - y0 - 2 * inset);
				float dx = fx - arcX, dy = fy - arcY;
				float radius = sqrtf(dx * dx + dy * dy) / s;
				float band = (kPlateH * 1.42f - radius) / 2.2f;	/* outer band first */
				tPixel color = MixRGB(238, 236, 226, 214, 214, 208, t);	/* aged white */
				if (pressed)
					color = MixRGB(222, 220, 210, 198, 198, 192, t);
				if (band >= 0 && band < 6) {
					int index = (int)band;
					float edge = band - index;
					/* soft seams between the colours */
					UInt32 alpha = (UInt32)(150 * (edge < 0.15f ? edge / 0.15f :
						(edge > 0.85f ? (1 - edge) / 0.15f : 1)));
					color = BlendAlpha(PixelRGB(kRainbow[index][0], kRainbow[index][1],
						kRainbow[index][2]), color, alpha);
				}
				*dst = BlendAlpha(color, *dst, (UInt32)(face * 255));
			}
		}
	/* screws in the top corners of the plate */
	{
		static const float kScrewX[2] = {0.24f, 0.76f};
		int screw;
		for (screw = 0; screw < 2; screw++) {
			float cx = x0 + kPlateW * kScrewX[screw] * s, cy = y0 + 8.5f * s;
			for (py = (int)(cy - 4 * s); py <= (int)(cy + 4 * s); py++)
				for (px = (int)(cx - 4 * s); px <= (int)(cx + 4 * s); px++) {
					float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
					float d = sqrtf(dx * dx + dy * dy) / s;
					float coverage = 3.2f - d;
					tPixel color;
					if (coverage <= 0 || px < 0 || py < 0 || px >= width || py >= height)
						continue;
					if (coverage > 1)
						coverage = 1;
					color = MixRGB(235, 235, 240, 120, 120, 130, (dy / s + 3) / 6);
					if (fabsf(dx - dy) < 0.6f * s && d < 2.4f)
						color = PixelRGB(70, 70, 80);	/* the slot */
					pixels[py * stride + px] = BlendAlpha(color, pixels[py * stride + px],
						(UInt32)(coverage * 255));
				}
		}
	}
	/* lettering: state, embossed main word with its highlight, slogan */
	{
		float base = offset;
		tPixel navy = hover ? PixelRGB(24, 40, 130) : PixelRGB(30, 40, 96);
		TR_DrawStringSmooth(pixels, stride, kPlateX + base + kPlateW / 2 - 3 * 6.5f,
			kPlateY + base + 13, "HAWAII", 6.5f, 9, PixelRGB(50, 48, 52));
		TR_DrawStringSmooth(pixels, stride, kPlateX + base + kPlateW / 2 - 11 * 5.5f + 1,
			kPlateY + base + 26 + 1, "MULTIPLAYER", 11, 26, PixelRGB(170, 170, 176));
		TR_DrawStringSmooth(pixels, stride, kPlateX + base + kPlateW / 2 - 11 * 5.5f,
			kPlateY + base + 26, "MULTIPLAYER", 11, 26, navy);
		TR_DrawStringSmooth(pixels, stride, kPlateX + base + kPlateW / 2 - 11 * 2.75f,
			kPlateY + base + kPlateH - 16, "LOCAL - LAN", 5.5f, 8, PixelRGB(150, 50, 52));
	}
	WeatherPlate(pixels, stride, x0, y0, x1, y1, s);
}

static void DrawMenuLicensePlate(int imageIndex, int hover, int pressed)
{
	tPixel *pixels = imageIndex == 0 ? sMenuNormal :
		(imageIndex == 1 ? sMenuHover : sMenuPressed);
	DrawLicensePlate(pixels, Platform_GetRenderWidth(), hover, pressed);
}

/* ---- Multiplayer menu ----
   A big mode selector on top, the three ways to play, and buttons for the
   crate settings and the controls. Keys: M mode, K crates, P controls. */
static const char *const kModeNames[kGameModeCount] = {
	"CO-OP", "DUEL", "PACKAGES", "POLICE", "ZOMBIE", "WILDLIFE"
};
static const char *const kModeInfos[kGameModeCount] = {
	"NO POINTS FOR HITS - SHARE YOUR LIVES",
	"WEAPONS HIT RIVALS - 1000 PER TAKEDOWN",
	"PACKAGES ALL OVER - SWARMS HUNT THE FINDER",
	"COPS KEEP COMING - AND THEY RAM",
	"EVERY CAR ON THE ROAD HUNTS YOU",
	"ANIMALS CROSSING THE ROAD EVERYWHERE"
};
static const UInt16 kModeColors[kGameModeCount] = {
	COL_GREEN, COL_RED, COL_YELLOW, COL_CYAN, COL_WHITE, 0x7E00
};
static const UInt16 kModeShades[kGameModeCount] = {
	0x0100, 0x2400, 0x2520, 0x00E9, 0x18C6, 0x28A0
};
#define kModePanelY 92
#define kModePanelH 58
#define kMenuRowY 160
#define kMenuRowStep 44
#define kMenuButtonY 300
#define kMenuButtonH 50
#define kMenuButtonW 132
#define kCratesButtonX 116
#define kLevelButtonX 254
#define kControlsButtonX 392

static int CurrentMultiplayerMode(void)
{
	return gPrefs.unused[8] < kGameModeCount ? gPrefs.unused[8] : 0;
}

/* A framed button: bright border, dark face, big label and two info lines. */
static void DrawMenuButton(tPixel *fb, int stride, int x, int y, int w, int h,
	UInt16 border, UInt16 face, const char *label, const char *info1,
	const char *info2, UInt16 labelColor)
{
	TR_FillRect(fb, stride, x + 3, y + 3, w, h, COL_BLACK);
	TR_FillRect(fb, stride, x, y, w, h, border);
	TR_FillRect(fb, stride, x + 2, y + 2, w - 4, h - 4, face);
	TR_FillRect(fb, stride, x + 2, y + 2, w - 4, 2, COL_GRAY);
	TR_DrawStringCentered(fb, stride, x + w / 2, y + 7, label, labelColor, 2);
	if (info1)
		TR_DrawStringCentered(fb, stride, x + w / 2, y + 27, info1, COL_WHITE, 1);
	if (info2)
		TR_DrawStringCentered(fb, stride, x + w / 2, y + 38, info2, labelColor, 1);
}

static void DrawMultiplayerOverlay(const char *title, const char *subtitle,
	int selected)
{
	static const char *labels[] = {"", "HOST LAN GAME", "JOIN LAN GAME"};
	static const char *kAmounts[kPackageAmountCount] = {"FEW", "NORMAL", "MORE", "MANY"};
	int rowBytes, stride, row, mode = CurrentMultiplayerMode();
	char info1[48], info2[48];
	const char *gasKey = SDL_GetScancodeName(
		(SDL_Scancode)gPrefs.localKeyCodes[0][kForward]);
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 96, 34, 448, 382, COL_DKGRAY);
	TR_FillRect(fb, stride, 100, 38, 440, 374, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 48, title, COL_YELLOW, 2);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 72, subtitle, COL_WHITE, 1);

	/* game mode selector */
	TR_FillRect(fb, stride, 119, kModePanelY + 3, 406, kModePanelH, COL_BLACK);
	TR_FillRect(fb, stride, 116, kModePanelY, 408, kModePanelH, kModeColors[mode]);
	TR_FillRect(fb, stride, 119, kModePanelY + 3, 402, kModePanelH - 6, kModeShades[mode]);
	TR_DrawString(fb, stride, 128, kModePanelY + 14, "<", kModeColors[mode], 3);
	TR_DrawString(fb, stride, 488, kModePanelY + 14, ">", kModeColors[mode], 3);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2 + 2, kModePanelY + 11,
		kModeNames[mode], COL_BLACK, 3);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, kModePanelY + 9,
		kModeNames[mode], kModeColors[mode], 3);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, kModePanelY + 42,
		kModeInfos[mode], COL_WHITE, 1);

	/* the three ways to play */
	for (row = 0; row < 3; ++row) {
		int y = kMenuRowY + row * kMenuRowStep;
		UInt16 color = row == selected ? COL_BAR_FG : COL_DKGRAY;
		TR_FillRect(fb, stride, 154, y, 332, 38, color);
		TR_FillRect(fb, stride, 156, y + 2, 328, 34,
			row == selected ? 0x7FE0 : COL_BAR_BG);
		if (row == 0) {
			int player;
			TR_DrawStringCentered(fb, stride, 232, y + 11, "LOCAL GAME",
				row == selected ? COL_BLACK : COL_WHITE, 1);
			for (player = 1; player <= kLocalPlayerCount; ++player) {
				int x = 318 + (player - 1) * 40;
				UInt16 numberColor = player == gPrefs.localPlayerCount ?
					COL_YELLOW : COL_WHITE;
				char number[2] = {(char)('0' + player), '\0'};
				TR_FillRect(fb, stride, x, y + 4, 34, 30,
					player == gPrefs.localPlayerCount ? COL_BAR_FG : COL_DKGRAY);
				TR_DrawStringCentered(fb, stride, x + 17, y + 15, number, numberColor, 1);
			}
		} else {
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y + 11,
				labels[row], row == selected ? COL_BLACK : COL_WHITE, 1);
		}
	}

	/* crate settings, start level and controls */
	{
		int themes = Network_GetLocalThemeMode(), kind, enabled = 0;
		for (kind = 0; kind < kCrateKindCount; ++kind)
			enabled += Network_GetLocalCrateWeight(kind) != kCrateOff;
		if (themes == kThemeRandom)
			snprintf(info1, sizeof(info1), "RANDOM THEMES");
		else if (themes >= kThemeFixed)
			snprintf(info1, sizeof(info1), "%s", kCrateThemes[themes - kThemeFixed].name);
		else
			snprintf(info1, sizeof(info1), "%d/%d CRATES ON", enabled, kCrateKindCount);
		snprintf(info2, sizeof(info2), "PACKAGES %s", kAmounts[Network_GetLocalPackageAmount()]);
		DrawMenuButton(fb, stride, kCratesButtonX, kMenuButtonY, kMenuButtonW, kMenuButtonH,
			COL_YELLOW, 0x2520, "CRATES", info1, info2, COL_YELLOW);
	}
	if (gLevelGenOn) {
		snprintf(info1, sizeof(info1), "GENERATED");
		LevelGen_Encode(&gLevelGen, info2);
	} else {
		snprintf(info1, sizeof(info1), "START IN %d", Prefs_GetStartLevel() + 1);
		snprintf(info2, sizeof(info2), "ORIGINAL LEVELS");
	}
	DrawMenuButton(fb, stride, kLevelButtonX, kMenuButtonY, kMenuButtonW, kMenuButtonH,
		0x7C1F, 0x300C, "LEVEL", info1, info2, 0x7C1F);
	snprintf(info1, sizeof(info1), "P1 GAS: %.8s", gasKey && gasKey[0] ? gasKey : "UNBOUND");
	DrawMenuButton(fb, stride, kControlsButtonX, kMenuButtonY, kMenuButtonW, kMenuButtonH,
		COL_CYAN, 0x00E9, "KEYS", info1, "ALL PLAYERS", COL_CYAN);

	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 364,
		"M MODE  K CRATES  G LEVEL  P KEYS  ENTER START", 0x4210, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 384,
		"ESC  BACK", 0x4210, 1);
	Platform_Blit2Screen();
}

/* ---- Crate settings (K in the multiplayer menu) ----
   Row 0: package amount, row 1: themes (off, random, or one theme), row 2:
   which theme's mix the list shows, then every crate kind with its
   frequency in two columns. With themes off the list is your own mix;
   otherwise it is the theme's, and changing it changes the theme. */
#define kCrateRows ((kCrateKindCount + 1) / 2)
#define kCrateListY 198
#define kCrateRowH 15
#define kCrateFirstRow 3

static const char *const kWeightNames[kCrateWeightCount] = {"OFF", "RARE", "NORMAL", "OFTEN"};
static const UInt16 kWeightColors[kCrateWeightCount] = {COL_GRAY, COL_CYAN, COL_WHITE, COL_YELLOW};

typedef struct {
	UInt8 weights[kCrateKindCount];
	UInt8 themeWeights[kCrateThemeCount][kCrateKindCount];
	int amount, themes, editTheme;
} tCrateEdit;

/* The list on screen: your own mix, or the theme being edited. */
static UInt8 *CrateEditList(tCrateEdit *edit)
{
	if (edit->themes == kThemeOff)
		return edit->weights;
	return edit->themeWeights[edit->themes >= kThemeFixed ?
		edit->themes - kThemeFixed : edit->editTheme];
}

static void DrawCrateRow(tPixel *fb, int stride, int y, int selected, const char *text)
{
	TR_FillRect(fb, stride, 92, y, 456, 18, selected ? COL_BAR_FG : COL_DKGRAY);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y + 5, text,
		selected ? COL_YELLOW : COL_WHITE, 1);
}

static void DrawCrateSettings(int selected, tCrateEdit *edit)
{
	static const char *kAmounts[kPackageAmountCount] = {
		"FEW - HALF THE PACKAGES", "NORMAL - THE LEVEL'S OWN",
		"MORE - ONE EVERY 640 PIXELS", "MANY - ONE EVERY 320 PIXELS"
	};
	int rowBytes, stride, kind;
	char line[80];
	const UInt8 *list = CrateEditList(edit);
	int theme = edit->themes >= kThemeFixed ? edit->themes - kThemeFixed : edit->editTheme;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes / 4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 72, 70, 496, 348, COL_DKGRAY);
	TR_FillRect(fb, stride, 76, 74, 488, 340, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 84, "CRATES", COL_YELLOW, 2);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 108,
		"PACKAGES AND HOW OFTEN EACH CRATE COMES (ANY MODE)", COL_WHITE, 1);
	snprintf(line, sizeof(line), "< PACKAGES: %s >", kAmounts[edit->amount]);
	DrawCrateRow(fb, stride, 126, selected == 0, line);
	if (edit->themes == kThemeRandom)
		snprintf(line, sizeof(line), "< THEMES: RANDOM - EACH LEVEL ONE OF THEM >");
	else if (edit->themes >= kThemeFixed)
		snprintf(line, sizeof(line), "< THEMES: ALWAYS %s >", kCrateThemes[theme].name);
	else
		snprintf(line, sizeof(line), "< THEMES: OFF - YOUR OWN MIX BELOW >");
	DrawCrateRow(fb, stride, 148, selected == 1, line);
	if (edit->themes == kThemeOff)
		snprintf(line, sizeof(line), "YOUR OWN MIX");
	else
		snprintf(line, sizeof(line), "< EDIT THEME: %s %d/%d >", kCrateThemes[theme].name,
			theme + 1, kCrateThemeCount);
	DrawCrateRow(fb, stride, 170, selected == 2, line);
	for (kind = 0; kind < kCrateKindCount; ++kind) {
		int column = kind / kCrateRows, row = kind % kCrateRows;
		int x = 92 + column * 232, y = kCrateListY + row * kCrateRowH;
		int weight = list[kind];
		if (selected == kind + kCrateFirstRow)
			TR_FillRect(fb, stride, x, y - 3, 224, kCrateRowH - 1, COL_BAR_FG);
		TR_DrawString(fb, stride, x + 6, y, kCrateKindNames[kind],
			weight ? COL_WHITE : COL_GRAY, 1);
		TR_DrawStringRight(fb, stride, x + 218, y, kWeightNames[weight], kWeightColors[weight], 1);
	}
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 368,
		"UP/DOWN SELECT  |  LEFT/RIGHT/SPACE CHANGE", COL_CYAN, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 382,
		edit->themes == kThemeOff ? "A: ALL NORMAL  |  N: ALL OFF  |  O: ORIGINAL ONLY" :
			"A: ALL NORMAL  |  N: ALL OFF  |  R: RESET THEME", COL_CYAN, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 398,
		"ESC  BACK (SAVED)", COL_GRAY, 1);
	Platform_Blit2Screen();
}

static void ChooseCrates(void)
{
	tCrateEdit edit;
	int selected = 0, kind, theme, rows = kCrateKindCount + kCrateFirstRow;
	for (kind = 0; kind < kCrateKindCount; ++kind) {
		edit.weights[kind] = (UInt8)Network_GetLocalCrateWeight(kind);
		for (theme = 0; theme < kCrateThemeCount; ++theme)
			edit.themeWeights[theme][kind] = (UInt8)Network_GetLocalThemeWeight(theme, kind);
	}
	edit.amount = Network_GetLocalPackageAmount();
	edit.themes = Network_GetLocalThemeMode();
	edit.editTheme = edit.themes >= kThemeFixed ? edit.themes - kThemeFixed : 0;
	DrawCrateSettings(selected, &edit);
	for (;;) {
		int x, y, change = 0, redraw = 0;
		UInt8 *list = CrateEditList(&edit);
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			break;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			break;
		if (Platform_WasKeyPressed(SDL_SCANCODE_UP)) {
			selected = (selected + rows - 1) % rows;
			redraw = 1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_DOWN)) {
			selected = (selected + 1) % rows;
			redraw = 1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_LEFT))
			change = -1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RIGHT) ||
			Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
			Platform_WasKeyPressed(SDL_SCANCODE_SPACE))
			change = 1;
		if (Platform_GetMouseClick(&x, &y)) {
			if (y >= 126 && y < 144)
				selected = 0, change = 1;
			else if (y >= 148 && y < 166)
				selected = 1, change = 1;
			else if (y >= 170 && y < 188)
				selected = 2, change = x < SCREEN_WIDTH / 2 ? -1 : 1;
			else if (y >= kCrateListY - 3 && x >= 92 && x < 548) {
				int column = (x - 92) / 232, row = (y - kCrateListY + 3) / kCrateRowH;
				kind = column * kCrateRows + row;
				if (row < kCrateRows && kind < kCrateKindCount)
					selected = kind + kCrateFirstRow, change = 1;
			}
			redraw = 1;
		}
		if (change) {
			if (selected == 0)
				edit.amount = (edit.amount + kPackageAmountCount + change) % kPackageAmountCount;
			else if (selected == 1) {
				edit.themes = (edit.themes + kThemeFixed + kCrateThemeCount + change) %
					(kThemeFixed + kCrateThemeCount);
				if (edit.themes >= kThemeFixed)
					edit.editTheme = edit.themes - kThemeFixed;
			} else if (selected == 2) {
				/* browse the themes; with one theme always on, switch it */
				if (edit.themes == kThemeRandom)
					edit.editTheme = (edit.editTheme + kCrateThemeCount + change) % kCrateThemeCount;
				else if (edit.themes >= kThemeFixed) {
					edit.editTheme = (edit.editTheme + kCrateThemeCount + change) % kCrateThemeCount;
					edit.themes = kThemeFixed + edit.editTheme;
				}
			} else {
				kind = selected - kCrateFirstRow;
				list[kind] = (UInt8)((list[kind] + kCrateWeightCount + change) % kCrateWeightCount);
			}
			redraw = 1;
		}
		list = CrateEditList(&edit);
		if (Platform_WasKeyPressed(SDL_SCANCODE_A)) {
			memset(list, kCrateNormal, kCrateKindCount);
			redraw = 1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_N)) {
			memset(list, kCrateOff, kCrateKindCount);
			redraw = 1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_O) && edit.themes == kThemeOff) {
			for (kind = 0; kind < kCrateKindCount; ++kind)
				list[kind] = kind < kCrateKindGrip ? kCrateNormal : kCrateOff;
			redraw = 1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_R) && edit.themes != kThemeOff) {
			theme = edit.themes >= kThemeFixed ? edit.themes - kThemeFixed : edit.editTheme;
			memcpy(edit.themeWeights[theme], kCrateThemes[theme].weights, kCrateKindCount);
			redraw = 1;
		}
		if (redraw)
			DrawCrateSettings(selected, &edit);
		SDL_Delay(16);
	}
	Network_SetLocalCrates(edit.weights, edit.amount, edit.themes,
		(const UInt8 (*)[kCrateKindCount])edit.themeWeights);
}

/* gPrefs.unused[8]: the multiplayer game mode, see kGameModeCoop... */
static void ToggleMultiplayerMode(int step)
{
	gPrefs.unused[8] = (UInt8)((CurrentMultiplayerMode() + kGameModeCount + step) %
		kGameModeCount);
	WritePrefs(0);
}

static void StepStartLevel(int step)
{
	Prefs_SetStartLevel((Prefs_GetStartLevel() + 10 + step) % 10);
	WritePrefs(0);
}

/* ---- LEVEL (G in the multiplayer menu): the original levels, or the level
   generator: a seed and track settings, written as a level code that gives
   the same level again on any computer. ---- */
#define kLevelRowY 66
#define kLevelRowH 16
#define kPreviewX 92
#define kPreviewY 342
#define kPreviewW 456
#define kPreviewH 54
enum {
	kLevelRowSource, kLevelRowFirstSetting,	/* landscape, then 8 settings */
	kLevelRowSeed = kLevelRowFirstSetting + 12, kLevelRowCode, kLevelRowSave,
	kLevelRowSaved, kLevelRowCount
};
static const char *const kSettingLabels[11] = {
	"LENGTH", "CURVES", "ROAD", "TRAFFIC", "TRAFFIC SPEED", "POLICE", "JUMPS", "TIME LIMIT",
	"CROSSINGS", "FORKS", "HIGHWAYS"
};

static UInt8 *GenSetting(tLevelGen *gen, int setting)
{
	UInt8 *values[11] = {&gen->length, &gen->curves, &gen->width, &gen->traffic,
		&gen->speed, &gen->cops, &gen->jumps, &gen->time, &gen->crossings, &gen->forks,
		&gen->highways};
	return values[setting];
}

/* The whole track as a map: start on the left, finish on the right, the
   road as seen from above (turned sideways), medians, ramps. */
static void DrawTrackPreview(tPixel *fb, int stride)
{
	static UInt32 cachedSeed, cachedPacked;
	static int cached, rampCount, bridgeCount;
	static float left[kPreviewW], right[kPreviewW], ramps[32], bridges[16];
	static UInt8 divided[kPreviewW];
	static const UInt16 kGround[kGenLandscapes] = {
		0x0D04, 0x0D04, 0x2D06, 0x0D04, 0x0D04, 0x0D04, 0x5EF7, 0x0442, 0x2D06, 0x5AE0
	};
	float low = 1e9f, high = -1e9f, scale;
	int column, i, water = gLevelGen.landscape == 4;
	if ((gLevelGen.crossings || gLevelGen.forks || gLevelGen.highways) && !water) {
		/* a free-form road: its real shape, loops and bridges, turned
		   sideways (start left, finish right) */
		static float xs[900], ys[900];
		static UInt8 bridge[900];
		static UInt32 routeSeed, routePacked;
		static int routeCount, routeKnown;
		float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, midX;
		int pass;
		if (!routeKnown || routeSeed != gLevelGen.seed || routePacked != LevelGen_Pack(&gLevelGen)) {
			routeCount = LevelGen_PreviewRoute(&gLevelGen, xs, ys, bridge, 900);
			routeKnown = 1;
			routeSeed = gLevelGen.seed;
			routePacked = LevelGen_Pack(&gLevelGen);
		}
		TR_FillRect(fb, stride, kPreviewX - 2, kPreviewY - 2, kPreviewW + 4, kPreviewH + 4, COL_DKGRAY);
		TR_FillRect(fb, stride, kPreviewX, kPreviewY, kPreviewW, kPreviewH, kGround[gLevelGen.landscape]);
		for (i = 0; i < routeCount; ++i) {
			if (xs[i] < minX) minX = xs[i];
			if (xs[i] > maxX) maxX = xs[i];
			if (ys[i] < minY) minY = ys[i];
			if (ys[i] > maxY) maxY = ys[i];
		}
		/* stretched to fill the map both ways: east-west highways and loops
		   would leave the rest of the road a tiny scribble otherwise */
		scale = (kPreviewW - 8) / (maxY - minY > 1 ? maxY - minY : 1);
		{
			float scaleX = (kPreviewH - 10) / (maxX - minX > 1 ? maxX - minX : 1);
			midX = (minX + maxX) * 0.5f;
			for (pass = 0; pass < 2; ++pass)	/* the road, then the bridges on top */
				for (i = 0; i < routeCount; ++i) {
					int px = kPreviewX + 4 + (int)((ys[i] - minY) * scale);
					int py = kPreviewY + kPreviewH / 2 - (int)((xs[i] - midX) * scaleX);
					if (pass == 0)
						TR_FillRect(fb, stride, px - 1, py - 1, 3, 3,
							bridge[i] == 2 ? 0x318C : 0x4A52);	/* highways darker */
					else if (bridge[i] == 1)
						TR_FillRect(fb, stride, px - 2, py - 2, 5, 5, 0x6B5A);
				}
		}
		TR_FillRect(fb, stride, kPreviewX, kPreviewY, 3, kPreviewH, COL_GREEN);
		TR_FillRect(fb, stride, kPreviewX + kPreviewW - 3, kPreviewY, 3, kPreviewH, COL_RED);
		TR_DrawString(fb, stride, kPreviewX + 6, kPreviewY + 3, "START", COL_WHITE, 1);
		TR_DrawStringRight(fb, stride, kPreviewX + kPreviewW - 6, kPreviewY + 3, "FINISH", COL_WHITE, 1);
		return;
	}
	if (!cached || cachedSeed != gLevelGen.seed || cachedPacked != LevelGen_Pack(&gLevelGen)) {
		if (!LevelGen_Preview(&gLevelGen, kPreviewW, left, right, divided, ramps, 32, &rampCount,
			bridges, &bridgeCount))
			return;
		cached = 1;
		cachedSeed = gLevelGen.seed;
		cachedPacked = LevelGen_Pack(&gLevelGen);
	}
	TR_FillRect(fb, stride, kPreviewX - 2, kPreviewY - 2, kPreviewW + 4, kPreviewH + 4, COL_DKGRAY);
	TR_FillRect(fb, stride, kPreviewX, kPreviewY, kPreviewW, kPreviewH,
		water ? 0x1CE0 : kGround[gLevelGen.landscape]);
	for (column = 0; column < kPreviewW; ++column) {
		if (left[column] < low) low = left[column];
		if (right[column] > high) high = right[column];
	}
	{
		float lengthScale = kPreviewW / (float)(gLevelGen.length == 0 ? 20000 :
			gLevelGen.length == 1 ? 32000 : gLevelGen.length == 2 ? 44000 : 58000);
		float sideScale = (kPreviewH - 6) / (high - low > 1 ? high - low : 1);
		/* bends drawn higher than they are (up to 8x), so they show */
		scale = sideScale < lengthScale * 8 ? sideScale : lengthScale * 8;
	}
	for (column = 0; column < kPreviewW; ++column) {
		float mid = (low + high) * 0.5f;
		/* right of the road is up on the map: the way the cars look */
		int top = kPreviewY + kPreviewH / 2 - (int)((right[column] - mid) * scale + 0.5f);
		int bottom = kPreviewY + kPreviewH / 2 - (int)((left[column] - mid) * scale - 0.5f);
		if (bottom - top < 3) {	/* at least a visible line */
			int middle = (top + bottom) / 2;
			top = middle - 1;
			bottom = middle + 2;
		}
		TR_FillRect(fb, stride, kPreviewX + column, top, 1, bottom - top,
			water ? 0x2D7F : divided[column] ? 0x5294 : 0x4A52);
		if (divided[column] && bottom - top > 3)
			TR_FillRect(fb, stride, kPreviewX + column, (top + bottom) / 2, 1, 1, 0x0D04);
	}
	for (i = 0; i < rampCount; ++i) {
		int column2 = (int)(ramps[i] * (kPreviewW - 1));
		float mid = (low + high) * 0.5f;
		int y = kPreviewY + kPreviewH / 2 - (int)(((left[column2] + right[column2]) * 0.5f - mid) * scale);
		TR_FillRect(fb, stride, kPreviewX + column2 - 1, y - 1, 3, 3, COL_YELLOW);
	}
	/* overpasses: a road crossing the track */
	for (i = 0; i < bridgeCount; ++i)
		TR_FillRect(fb, stride, kPreviewX + (int)(bridges[i] * (kPreviewW - 1)) - 1, kPreviewY + 2,
			3, kPreviewH - 4, 0x6B5A);
	TR_FillRect(fb, stride, kPreviewX, kPreviewY, 3, kPreviewH, COL_GREEN);
	TR_FillRect(fb, stride, kPreviewX + kPreviewW - 3, kPreviewY, 3, kPreviewH, COL_RED);
	TR_DrawString(fb, stride, kPreviewX + 6, kPreviewY + 3, "START", COL_WHITE, 1);
	TR_DrawStringRight(fb, stride, kPreviewX + kPreviewW - 6, kPreviewY + 3, "FINISH", COL_WHITE, 1);
}

static void DrawLevelSettings(int selected, const char *message, UInt16 messageColor)
{
	int rowBytes, stride, row, rows = gLevelGenOn ? kLevelRowCount : kLevelRowFirstSetting + 1;
	char line[80], code[kLevelCodeSize];
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes / 4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 72, 26, 496, 428, COL_DKGRAY);
	TR_FillRect(fb, stride, 76, 30, 488, 420, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 38, "LEVEL", 0x7C1F, 2);
	LevelGen_Encode(&gLevelGen, code);
	for (row = 0; row < rows; ++row) {
		int y = kLevelRowY + row * kLevelRowH;
		if (row == kLevelRowSource)
			snprintf(line, sizeof(line), "< LEVELS: %s >", gLevelGenOn ? "GENERATED" : "ORIGINAL");
		else if (!gLevelGenOn)
			snprintf(line, sizeof(line), "< START IN LEVEL %d: %s >", Prefs_GetStartLevel() + 1,
				LevelGen_LandscapeName(Prefs_GetStartLevel()));
		else if (row == kLevelRowFirstSetting)
			snprintf(line, sizeof(line), "< LANDSCAPE: %s >", LevelGen_LandscapeName(gLevelGen.landscape));
		else if (row < kLevelRowSeed) {
			int setting = row - kLevelRowFirstSetting - 1;
			snprintf(line, sizeof(line), "< %s: %s >", kSettingLabels[setting],
				LevelGen_ChoiceName(setting, *GenSetting(&gLevelGen, setting)));
		} else if (row == kLevelRowSeed)
			snprintf(line, sizeof(line), "SEED: %u   (DIGITS, R: RANDOM)", (unsigned)gLevelGen.seed);
		else if (row == kLevelRowCode)
			snprintf(line, sizeof(line), "LEVEL CODE: %s   (ENTER: TYPE ONE)", code);
		else if (row == kLevelRowSave)
			snprintf(line, sizeof(line), "SAVE THIS LEVEL");
		else
			snprintf(line, sizeof(line), "SAVED LEVELS (%d)", LevelGen_SavedCount());
		DrawCrateRow(fb, stride, y, row == selected, line);
	}
	if (gLevelGenOn)
		DrawTrackPreview(fb, stride);
	if (message)
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 400, message, messageColor, 1);
	else if (gLevelGenOn)
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 400,
			"WRITE THE CODE DOWN OR SAVE IT: IT GIVES THIS LEVEL AGAIN", COL_GRAY, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 414,
		"UP/DOWN SELECT  |  LEFT/RIGHT CHANGE  |  ENTER", COL_CYAN, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 427,
		gLevelGenOn ? "A LAN GAME USES THE HOST'S LEVELS" : "THE ORIGINAL TEN LEVELS", COL_CYAN, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 440, "ESC  BACK (SAVED)", COL_GRAY, 1);
	Platform_Blit2Screen();
}

/* Typing a level code; returns 1 with gLevelGen set from a valid one. */
static int EnterLevelCode(void)
{
	char text[20] = "";
	const char *error = NULL;
	Platform_StartTextEntry();
	for (;;) {
		int rowBytes, stride;
		char typed[20], shown[40];
		size_t length = strlen(text);
		tPixel *fb;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			break;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			break;
		if (Platform_TakeTypedText(typed, sizeof(typed))) {
			const char *c;
			for (c = typed; *c && length + 1 < sizeof(text) - 4; ++c)
				if ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
					(*c >= 'a' && *c <= 'z') || *c == '-') {
					text[length++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c);
					text[length] = '\0';
				}
			error = NULL;
		}
		if (Platform_WasKeyTyped(SDL_SCANCODE_BACKSPACE) && length)
			text[--length] = '\0';
		if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER)) {
			tLevelGen gen;
			if (LevelGen_Decode(text, &gen)) {
				gLevelGen = gen;
				Platform_StopTextEntry();
				return 1;
			}
			error = "NOT A LEVEL CODE - CHECK THE LETTERS";
		}
		fb = Platform_GetFramebuffer(&rowBytes);
		if (fb) {
			stride = rowBytes / 4;
			TR_FillRect(fb, stride, 112, 160, 416, 150, COL_DKGRAY);
			TR_FillRect(fb, stride, 116, 164, 408, 142, COL_BAR_BG);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 178, "TYPE A LEVEL CODE", 0x7C1F, 2);
			snprintf(shown, sizeof(shown), "%s%s", text, (SDL_GetTicks64() / 400) & 1 ? "_" : " ");
			TR_FillRect(fb, stride, 150, 212, 340, 26, COL_BLACK);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 216, shown, COL_GREEN, 2);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 252,
				error ? error : "LIKE K7Q2-M9XA-3ZPD", error ? COL_RED : COL_GRAY, 1);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 280, "ENTER: OK   ESC: BACK", COL_CYAN, 1);
			Platform_Blit2Screen();
		}
		SDL_Delay(16);
	}
	Platform_StopTextEntry();
	return 0;
}

/* The saved levels: ENTER loads one, DELETE removes it. */
static int ChooseSavedLevel(void)
{
	int selected = 0;
	for (;;) {
		int rowBytes, stride, count = LevelGen_SavedCount(), i, x, y;
		tPixel *fb;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			return 0;
		if (count && Platform_WasKeyPressed(SDL_SCANCODE_UP))
			selected = (selected + count - 1) % count;
		if (count && Platform_WasKeyPressed(SDL_SCANCODE_DOWN))
			selected = (selected + 1) % count;
		if (selected >= count)
			selected = count ? count - 1 : 0;
		if (count && (Platform_WasKeyPressed(SDL_SCANCODE_DELETE) ||
			Platform_WasKeyPressed(SDL_SCANCODE_BACKSPACE)))
			LevelGen_DeleteSaved(selected);
		if (count && (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER)))
			return LevelGen_GetSaved(selected, &gLevelGen);
		if (Platform_GetMouseClick(&x, &y)) {
			i = (y - 96) / kLevelRowH;
			if (y >= 96 && i < count) {
				if (i == selected)
					return LevelGen_GetSaved(selected, &gLevelGen);
				selected = i;
			}
		}
		fb = Platform_GetFramebuffer(&rowBytes);
		if (fb) {
			stride = rowBytes / 4;
			TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
			TR_FillRect(fb, stride, 72, 46, 496, 388, COL_DKGRAY);
			TR_FillRect(fb, stride, 76, 50, 488, 380, COL_BAR_BG);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 62, "SAVED LEVELS", 0x7C1F, 2);
			if (!count)
				TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 200,
					"NONE YET - SAVE ONE IN THE LEVEL SCREEN", COL_GRAY, 1);
			for (i = 0; i < count; ++i) {
				tLevelGen gen;
				char code[kLevelCodeSize], line[80];
				LevelGen_GetSaved(i, &gen);
				LevelGen_Encode(&gen, code);
				snprintf(line, sizeof(line), "%s  %s  %s", code, LevelGen_LandscapeName(gen.landscape),
					LevelGen_ChoiceName(0, gen.length));
				DrawCrateRow(fb, stride, 96 + i * kLevelRowH, i == selected, line);
			}
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 400,
				"ENTER: PLAY THIS ONE  |  DELETE: REMOVE", COL_CYAN, 1);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 416, "ESC  BACK", COL_GRAY, 1);
			Platform_Blit2Screen();
		}
		SDL_Delay(16);
	}
}

static void ChangeLevelRow(int row, int step)
{
	if (row == kLevelRowSource)
		gLevelGenOn = !gLevelGenOn;
	else if (!gLevelGenOn)
		Prefs_SetStartLevel((Prefs_GetStartLevel() + 10 + step) % 10);
	else if (row == kLevelRowFirstSetting)
		gLevelGen.landscape = (UInt8)((gLevelGen.landscape + kGenLandscapes + step) % kGenLandscapes);
	else if (row < kLevelRowSeed) {
		UInt8 *value = GenSetting(&gLevelGen, row - kLevelRowFirstSetting - 1);
		*value = (UInt8)((*value + kGenChoices + step) % kGenChoices);
	}
}

static void ChooseLevelSettings(void)
{
	int selected = 0, seedTyped = 0;
	const char *message = NULL;
	UInt16 messageColor = COL_GREEN;
	/* every change is kept at once (not only when leaving the screen) */
	UInt32 keptPack = LevelGen_Pack(&gLevelGen) ^ (gLevelGenOn ? 0x80000000u : 0), keptSeed = gLevelGen.seed;
	for (;;) {
		int rows = gLevelGenOn ? kLevelRowCount : kLevelRowFirstSetting + 1, x, y, digit, activate = 0;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			break;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			break;
		if (selected >= rows)
			selected = rows - 1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_UP)) {
			selected = (selected + rows - 1) % rows;
			seedTyped = 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_DOWN)) {
			selected = (selected + 1) % rows;
			seedTyped = 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_LEFT))
			ChangeLevelRow(selected, -1), message = NULL;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RIGHT))
			ChangeLevelRow(selected, 1), message = NULL;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
			Platform_WasKeyPressed(SDL_SCANCODE_SPACE))
			activate = 1;
		if (gLevelGenOn && Platform_WasKeyPressed(SDL_SCANCODE_R)) {
			gLevelGen.seed = (UInt32)(SDL_GetPerformanceCounter() * 2654435761u % 1000000000u);
			message = NULL;
		}
		/* digits on the seed row type a new seed */
		if (gLevelGenOn && selected == kLevelRowSeed) {
			for (digit = 0; digit < 10; ++digit)
				if (Platform_WasKeyPressed(digit ? SDL_SCANCODE_1 + digit - 1 : SDL_SCANCODE_0) ||
					Platform_WasKeyPressed(digit ? SDL_SCANCODE_KP_1 + digit - 1 : SDL_SCANCODE_KP_0)) {
					UInt64 seed = seedTyped ? (UInt64)gLevelGen.seed * 10 + digit : (UInt64)digit;
					if (seed <= 0xFFFFFFFFull)
						gLevelGen.seed = (UInt32)seed;
					seedTyped = 1;
					message = NULL;
				}
			if (Platform_WasKeyTyped(SDL_SCANCODE_BACKSPACE)) {
				gLevelGen.seed /= 10;
				seedTyped = 1;
			}
		}
		if (Platform_GetMouseClick(&x, &y)) {
			int row = (y - kLevelRowY) / kLevelRowH;
			if (y >= kLevelRowY && row < rows && x >= 92 && x < 548) {
				selected = row;
				seedTyped = 0;
				if (row < kLevelRowSeed)
					ChangeLevelRow(row, x < SCREEN_WIDTH / 2 ? -1 : 1);
				else
					activate = 1;
				message = NULL;
			} else if (y >= 424)
				break;
		}
		if (activate) {
			if (selected < kLevelRowSeed && selected != kLevelRowSeed)
				ChangeLevelRow(selected, 1);
			else if (selected == kLevelRowCode) {
				if (EnterLevelCode())
					message = "LEVEL CODE LOADED", messageColor = COL_GREEN;
			} else if (selected == kLevelRowSave) {
				LevelGen_AddSaved(&gLevelGen);
				message = "SAVED - FIND IT UNDER SAVED LEVELS", messageColor = COL_GREEN;
			} else if (selected == kLevelRowSaved) {
				if (ChooseSavedLevel())
					message = "SAVED LEVEL LOADED", messageColor = COL_GREEN;
			}
		}
		if ((LevelGen_Pack(&gLevelGen) ^ (gLevelGenOn ? 0x80000000u : 0)) != keptPack || gLevelGen.seed != keptSeed) {
			LevelGen_SaveSettings();
			keptPack = LevelGen_Pack(&gLevelGen) ^ (gLevelGenOn ? 0x80000000u : 0);
			keptSeed = gLevelGen.seed;
		}
		DrawLevelSettings(selected, message, messageColor);
		SDL_Delay(16);
	}
	WritePrefs(0);
	LevelGen_SaveSettings();
}

/* The way of playing chosen last (local, host, join): preselected, so after
   a game Enter plays the same again. */
static int sLastMultiplayerChoice;
static int sReturnToMultiplayer;

void Interface_ReturnToMultiplayer(void)
{
	sReturnToMultiplayer = 1;
}

static int ChooseMultiplayerMode(void)
{
	int selected = sLastMultiplayerChoice;
	DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
	for (;;) {
		int x, y;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return -1;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			return -1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_P)) {
			ConfigureInput();
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_M)) {
			ToggleMultiplayerMode(1);
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_EQUALS) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_PLUS) ||
			Platform_WasKeyPressed(SDL_SCANCODE_RIGHTBRACKET)) {
			StepStartLevel(1);
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_MINUS) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_MINUS) ||
			Platform_WasKeyPressed(SDL_SCANCODE_SLASH)) {
			StepStartLevel(-1);
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_G)) {
			ChooseLevelSettings();
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_K)) {
			ChooseCrates();
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_UP) ||
			Platform_WasKeyPressed(SDL_SCANCODE_W)) {
			selected = (selected + 2) % 3;
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_DOWN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_S)) {
			selected = (selected + 1) % 3;
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
		}
		if (selected == 0 && Platform_WasKeyPressed(SDL_SCANCODE_LEFT)) {
			if (gPrefs.localPlayerCount <= 1)
				gPrefs.localPlayerCount = kLocalPlayerCount;
			else
				gPrefs.localPlayerCount--;
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
		}
		if (selected == 0 && Platform_WasKeyPressed(SDL_SCANCODE_RIGHT)) {
			if (gPrefs.localPlayerCount >= kLocalPlayerCount)
				gPrefs.localPlayerCount = 1;
			else
				gPrefs.localPlayerCount++;
			DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER))
			return selected;
		if (Platform_WasKeyPressed(SDL_SCANCODE_1) ||
			Platform_WasKeyPressed(SDL_SCANCODE_L))
			return 0;
		if (Platform_WasKeyPressed(SDL_SCANCODE_H))
			return 1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_J))
			return 2;
		if (Platform_GetMouseClick(&x, &y)) {
			int row;
			if (y >= kModePanelY && y < kModePanelY + kModePanelH && x >= 116 && x < 524) {
				ToggleMultiplayerMode(x < 180 ? -1 : 1);
				DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
				continue;
			}
			if (y >= kMenuButtonY && y < kMenuButtonY + kMenuButtonH) {
				if (x >= kCratesButtonX && x < kCratesButtonX + kMenuButtonW)
					ChooseCrates();
				else if (x >= kLevelButtonX && x < kLevelButtonX + kMenuButtonW)
					ChooseLevelSettings();
				else if (x >= kControlsButtonX && x < kControlsButtonX + kMenuButtonW)
					ConfigureInput();
				DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
				continue;
			}
			for (row = 0; row < 3; ++row) {
				int top = kMenuRowY + row * kMenuRowStep;
				if (x < 154 || x >= 486 || y < top || y >= top + 38)
					continue;
				if (row == 0 && x >= 318) {
					int player = 1 + (x - 318) / 40;
					if (player >= 1 && player <= kLocalPlayerCount) {
						gPrefs.localPlayerCount = (UInt8)player;
						WritePrefs(0);
						break;
					}
				}
				return row;
			}
			if (row < 3) {
				DrawMultiplayerOverlay("MULTIPLAYER", "CHOOSE HOW TO PLAY", selected);
				continue;
			}
			if (y >= 380 && y < 396) return -1;
		}
		SDL_Delay(16);
	}
}

/* LAN player count: 2 to kNetworkMaxPlayers drivers, one per computer. */
#define kCountButtonW 40
#define kCountButtonGap 4
#define kCountButtonY 196
#define kCountButtonH 56

static int CountButtonX(int count, int minimum)
{
	int buttons = kNetworkMaxPlayers - minimum + 1;
	int width = buttons * (kCountButtonW + kCountButtonGap) - kCountButtonGap;
	return SCREEN_WIDTH / 2 - width / 2 +
		(count - minimum) * (kCountButtonW + kCountButtonGap);
}

static void DrawPlayerCountPicker(int selected, int minimum, const char *title)
{
	int rowBytes, stride, count;
	char label[32];
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 112, 104, 416, 270, COL_DKGRAY);
	TR_FillRect(fb, stride, 116, 108, 408, 262, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 130,
		title, COL_YELLOW, 2);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 158,
		"CHOOSE NUMBER OF PLAYERS", COL_WHITE, 1);
	for (count = minimum; count <= kNetworkMaxPlayers; ++count) {
		int x = CountButtonX(count, minimum);
		char number[3];
		snprintf(number, sizeof(number), "%d", count);
		TR_FillRect(fb, stride, x, kCountButtonY, kCountButtonW, kCountButtonH,
			count == selected ? 0x7FE0 : COL_DKGRAY);
		TR_FillRect(fb, stride, x + 2, kCountButtonY + 2, kCountButtonW - 4,
			kCountButtonH - 4, count == selected ? COL_BAR_FG : COL_DKGRAY);
		TR_DrawStringCentered(fb, stride, x + kCountButtonW / 2,
			kCountButtonY + kCountButtonH / 2 - 8, number,
			count == selected ? COL_BLACK : COL_WHITE, 2);
	}
	snprintf(label, sizeof(label), "%d PLAYERS  -  ONE PER COMPUTER", selected);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 264,
		label, COL_GRAY, 1);
	TR_FillRect(fb, stride, 216, 286, 208, 38, COL_BAR_FG);
	TR_FillRect(fb, stride, 218, 288, 204, 34, 0x7FE0);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 299,
		"HOST LAN GAME", COL_BLACK, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 342,
		"ESC  BACK", COL_GRAY, 1);
	Platform_Blit2Screen();
}

static int ChoosePlayerCount(int minimum, const char *title)
{
	static int sLanPlayerCount = 2;
	int selected = sLanPlayerCount;
	if (selected < minimum || selected > kNetworkMaxPlayers)
		selected = minimum;
	DrawPlayerCountPicker(selected, minimum, title);
	for (;;) {
		int x, y;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			return 0;
		if (Platform_WasKeyPressed(SDL_SCANCODE_LEFT) ||
			Platform_WasKeyPressed(SDL_SCANCODE_A))
			selected = selected <= minimum ? kNetworkMaxPlayers : selected - 1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RIGHT) ||
			Platform_WasKeyPressed(SDL_SCANCODE_D))
			selected = selected >= kNetworkMaxPlayers ? minimum : selected + 1;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER))
			return sLanPlayerCount = selected;
		if (Platform_GetMouseClick(&x, &y)) {
			int count;
			for (count = minimum; count <= kNetworkMaxPlayers; ++count) {
				int buttonX = CountButtonX(count, minimum);
				if (x >= buttonX && x < buttonX + kCountButtonW &&
					y >= kCountButtonY && y < kCountButtonY + kCountButtonH) {
					selected = count;
					break;
				}
			}
			if (x >= 216 && x < 424 && y >= 286 && y < 324)
				return sLanPlayerCount = selected;
			if (y >= 330 && y < 358)
				return 0;
		}
		DrawPlayerCountPicker(selected, minimum, title);
		SDL_Delay(16);
	}
}

/* Drivers in the LAN lobby with the vehicle each one picked. */
/* The game settings of this LAN game: mode, crates and packages. They are
   the host's; a client shows what it received from the host. */
#define kLobbyPanelH 50

static void DrawLobbySettings(tPixel *fb, int stride, int y, int isClient)
{
	static const char *kAmounts[kPackageAmountCount] = {"FEW", "NORMAL", "MORE", "MANY"};
	int mode = Network_GetGameMode(), themes = Network_GetThemeMode(), kind, enabled = 0;
	char line[64];
	if (mode < 0 || mode >= kGameModeCount)
		mode = 0;
	for (kind = 0; kind < kCrateKindCount; ++kind)
		enabled += Network_GetCrateWeight(kind) != kCrateOff;
	TR_FillRect(fb, stride, 136, y, 368, kLobbyPanelH, kModeColors[mode]);
	TR_FillRect(fb, stride, 138, y + 2, 364, kLobbyPanelH - 4, kModeShades[mode]);
	TR_DrawString(fb, stride, 146, y + 6, isClient ? "SET BY THE HOST" : "GAME SETTINGS",
		COL_GRAY, 1);
	TR_DrawStringRight(fb, stride, 494, y + 6, kModeNames[mode], kModeColors[mode], 2);
	if (themes == kThemeRandom)
		snprintf(line, sizeof(line), "CRATES: RANDOM THEMES");
	else if (themes >= kThemeFixed)
		snprintf(line, sizeof(line), "CRATES: THEME %s", kCrateThemes[themes - kThemeFixed].name);
	else
		snprintf(line, sizeof(line), "CRATES: %d OF %d ON", enabled, kCrateKindCount);
	TR_DrawString(fb, stride, 146, y + 24, line, COL_WHITE, 1);
	snprintf(line, sizeof(line), "PACKAGES: %s", kAmounts[Network_GetPackageAmount()]);
	TR_DrawString(fb, stride, 146, y + 36, line, COL_WHITE, 1);
	{
		/* generated levels: the code, so everyone can note it down */
		tLevelGen gen;
		char code[kLevelCodeSize];
		if (Network_GetLevelGen(&gen)) {
			LevelGen_Encode(&gen, code);
			snprintf(line, sizeof(line), "LEVEL %s", code);
		} else
			snprintf(line, sizeof(line), "ORIGINAL LEVELS");
		TR_DrawStringRight(fb, stride, 494, y + 36, line, COL_YELLOW, 1);
	}
}

static void DrawLobbyDrivers(tPixel *fb, int stride, int y)
{
	int slot, drivers = 0, row = 0;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot)
		if (Network_GetSlotName(slot))
			drivers++;
	for (slot = 0; slot < kNetworkMaxPlayers; ++slot) {
		const char *name = Network_GetSlotName(slot);
		UInt16 color = slot == Network_GetLocalSlot() ? COL_YELLOW : COL_WHITE;
		char line[48];
		if (!name)
			continue;
		if (drivers <= 4) {
			snprintf(line, sizeof(line), "%d  %-15s %s", slot + 1, name,
				Vehicle_Name(Network_GetPlayerVehicle(slot)));
			TR_DrawString(fb, stride, 150, y + row * 15, line, color, 1);
		} else {
			/* Up to ten drivers: two columns of five. */
			snprintf(line, sizeof(line), "%2d %-10.10s %.12s", slot + 1, name,
				Vehicle_Name(Network_GetPlayerVehicle(slot)));
			TR_DrawString(fb, stride, row < 5 ? 122 : 322, y + (row % 5) * 13,
				line, color, 1);
		}
		row++;
	}
}

static void DrawVehiclePicker(tPixel *fb, int stride, int y)
{
	char line[40];
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y,
		"YOUR VEHICLE  (LEFT / RIGHT)", COL_GRAY, 1);
	snprintf(line, sizeof(line), "<  %s  >", Vehicle_Name(Network_GetLocalVehicle()));
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y + 14, line, COL_CYAN, 2);
}

/* LEFT/RIGHT or a click on the picker row changes the lobby vehicle. */
static void HandleVehiclePicker(int clickX, int clickY, int pickerY)
{
	int step = 0, index = Vehicle_IndexOf(Network_GetLocalVehicle());
	if (Platform_WasKeyPressed(SDL_SCANCODE_LEFT))
		step = -1;
	if (Platform_WasKeyPressed(SDL_SCANCODE_RIGHT))
		step = 1;
	if (clickY >= pickerY + 10 && clickY < pickerY + 34)
		step = clickX < SCREEN_WIDTH / 2 ? -1 : 1;
	if (step)
		Network_SetLocalVehicle(Vehicle_ID(index + step));
}

/* Lobby name field: N or a click edits this computer's driver name, which
   the others see in their lobby and over the car during the race. */
static int sNameEditing;

static void LocalName(char name[16])
{
	int length = gPrefs.playerNames[0][0];
	if (length > 15)
		length = 15;
	memcpy(name, gPrefs.playerNames[0] + 1, (size_t)length);
	name[length] = '\0';
}

static void DrawNameField(tPixel *fb, int stride, int y)
{
	char name[16], line[48];
	LocalName(name);
	if (sNameEditing) {
		TR_FillRect(fb, stride, 150, y - 3, 340, 15, COL_DKGRAY);
		snprintf(line, sizeof(line), "YOUR NAME: %s%s   ENTER: OK", name,
			(SDL_GetTicks64() / 400) & 1 ? "_" : " ");
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y, line, COL_GREEN, 1);
	} else {
		snprintf(line, sizeof(line), "YOUR NAME: %s   (N: CHANGE)",
			name[0] ? name : "DRIVER");
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y, line, COL_WHITE, 1);
	}
}

static void StopNameEdit(void)
{
	if (!sNameEditing)
		return;
	sNameEditing = 0;
	Platform_StopTextEntry();
	WritePrefs(0);
}

/* Returns 1 while the name is being typed: the lobby then ignores its other
   keys (ESC, V, arrows). */
static int HandleNameField(int clickX, int clickY, int y)
{
	int clickedField = clickY >= y - 4 && clickY < y + 12 &&
		clickX >= 150 && clickX < 490;
	if (!sNameEditing) {
		if (clickedField || Platform_WasKeyPressed(SDL_SCANCODE_N)) {
			sNameEditing = 1;
			Platform_StartTextEntry();
			return 1;
		}
		return 0;
	}
	{
		char name[16], typed[16];
		size_t length;
		int changed = 0;
		LocalName(name);
		length = strlen(name);
		if (Platform_TakeTypedText(typed, sizeof(typed)) && length < 15) {
			size_t add = strlen(typed);
			if (length + add > 15)
				add = 15 - length;
			memcpy(name + length, typed, add);
			length += add;
			name[length] = '\0';
			changed = 1;
		}
		if (Platform_WasKeyTyped(SDL_SCANCODE_BACKSPACE) && length) {
			name[--length] = '\0';
			changed = 1;
		}
		if (changed)
			Network_SetLocalName(name);
	}
	if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
		Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER) ||
		Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE) ||
		(clickY >= 0 && !clickedField)) {
		StopNameEdit();
		Platform_FlushInput();
	}
	return 1;
}

static void DrawLanViewChoice(tPixel *fb, int stride, int y)
{
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, y,
		Prefs_IsLanOwnScreen() ? "V: VIEW  OWN SCREEN" : "V: VIEW  SPLIT SCREEN",
		COL_CYAN, 1);
}

#define kHostPickerY 234
#define kHostStartY 316

static void DrawLanLobby(int targetPlayers, int connectedPlayers)
{
	int rowBytes, stride;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 112, 52, 416, 374, COL_DKGRAY);
	TR_FillRect(fb, stride, 116, 56, 408, 366, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 66,
		"LAN GAME LOBBY", COL_YELLOW, 2);
	{
		char status[48];
		snprintf(status, sizeof(status), "CONNECTED: %d / %d",
			connectedPlayers, targetPlayers);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 90,
			status, connectedPlayers >= targetPlayers ? COL_GREEN : COL_YELLOW, 1);
	}
	DrawLobbySettings(fb, stride, 106, 0);
	DrawLobbyDrivers(fb, stride, 166);
	DrawVehiclePicker(fb, stride, kHostPickerY);
	DrawNameField(fb, stride, 269);
	{
		char address[16], line[56];
		if (Network_GetLocalAddress(address, sizeof(address)))
			snprintf(line, sizeof(line), "OTHERS: JOIN LAN GAME (OR IP %s)", address);
		else
			snprintf(line, sizeof(line), "OTHERS: CHOOSE JOIN LAN GAME");
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 287, line, COL_GRAY, 1);
	}
	DrawLanViewChoice(fb, stride, 300);
	if (connectedPlayers >= targetPlayers) {
		TR_FillRect(fb, stride, 216, kHostStartY, 208, 40, COL_BAR_FG);
		TR_FillRect(fb, stride, 218, kHostStartY + 2, 204, 36, 0x7FE0);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, kHostStartY + 14,
			"START RACE", COL_BLACK, 1);
	} else {
		TR_FillRect(fb, stride, 216, kHostStartY, 208, 40, COL_DKGRAY);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, kHostStartY + 14,
			"WAITING FOR PLAYERS", COL_GRAY, 1);
	}
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 400,
		"ESC  CANCEL", COL_GRAY, 1);
	Platform_Blit2Screen();
}

static int WaitForLanPlayers(int targetPlayers)
{
	for (;;) {
		int x, y, connected, clicked;
		Platform_PollEvents();
		Network_Poll();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return 0;
		}
		connected = Network_GetPlayerCount();
		Network_Publish();	/* clients see the drivers and vehicles */
		DrawLanLobby(targetPlayers, connected);
		x = y = -1;
		clicked = Platform_GetMouseClick(&x, &y);
		if (HandleNameField(x, y, 269)) {
			SDL_Delay(16);
			continue;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			return 0;
		HandleVehiclePicker(-1, -1, kHostPickerY);
		if (Platform_WasKeyPressed(SDL_SCANCODE_V))
			Prefs_ToggleLanScreen();
		if ((Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER)) &&
			connected >= targetPlayers)
			return 1;
		if (clicked) {
			HandleVehiclePicker(x, y, kHostPickerY);
			if (y >= kHostStartY && y < kHostStartY + 40 && x >= 216 && x < 424 &&
				connected >= targetPlayers)
				return 1;
			if (y >= 394 && y < 414)
				return 0;
		}
		SDL_Delay(16);
	}
}

static void DrawHostList(const tLanHost *hosts, int count, int frame)
{
	int rowBytes, stride, i;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 112, 88, 416, 308, COL_DKGRAY);
	TR_FillRect(fb, stride, 116, 92, 408, 300, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 110,
		"JOIN LAN GAME", COL_YELLOW, 2);
	if (!count) {
		static const char *dots[] = {"", ".", "..", "..."};
		char line[48];
		snprintf(line, sizeof(line), "SEARCHING THE NETWORK%s", dots[(frame / 20) % 4]);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 190, line, COL_WHITE, 1);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 214,
			"THE HOST MUST BE IN THE LAN LOBBY", COL_GRAY, 1);
	}
	for (i = 0; i < count; ++i) {
		int y = 140 + i * 40;
		char label[48], players[24];
		int open = !hosts[i].started && hosts[i].players < hosts[i].limit;
		TR_FillRect(fb, stride, 140, y, 360, 34, open ? COL_BAR_FG : COL_DKGRAY);
		snprintf(label, sizeof(label), "%d  %s", i + 1,
			hosts[i].name[0] ? hosts[i].name : hosts[i].address);
		TR_DrawString(fb, stride, 150, y + 6, label, open ? COL_WHITE : COL_GRAY, 1);
		TR_DrawString(fb, stride, 150, y + 20, hosts[i].address, COL_GRAY, 1);
		if (hosts[i].started)
			snprintf(players, sizeof(players), "RACE RUNNING");
		else
			snprintf(players, sizeof(players), "%d/%d DRIVERS",
				hosts[i].players, hosts[i].limit);
		TR_DrawStringRight(fb, stride, 490, y + 13, players,
			open ? COL_YELLOW : COL_GRAY, 1);
	}
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 352,
		"CLICK OR 1-5: JOIN   M: ENTER IP", COL_CYAN, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 370,
		"ESC  BACK", COL_GRAY, 1);
	Platform_Blit2Screen();
}

/* Lists hosts found on the LAN. Returns 1 with the chosen address, 2 for
   manual address entry, 0 to go back. */
static int ChooseLanHost(char *address, size_t capacity)
{
	tLanHost hosts[5];
	int count = 0, frame = 0, choice = -1;
	if (!Network_StartDiscovery(kMultiplayerPort))
		return 2;
	while (choice < 0) {
		int x, y, i;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			choice = 0;
			break;
		}
		count = Network_PollDiscovery(hosts, 5);
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			choice = 0;
		if (Platform_WasKeyPressed(SDL_SCANCODE_M))
			choice = 2;
		for (i = 0; i < count && choice < 0; ++i)
			if (Platform_WasKeyPressed((SDL_Scancode)(SDL_SCANCODE_1 + i)) &&
				!hosts[i].started) {
				snprintf(address, capacity, "%s", hosts[i].address);
				choice = 1;
			}
		if (choice < 0 && Platform_GetMouseClick(&x, &y)) {
			for (i = 0; i < count; ++i)
				if (x >= 140 && x < 500 && y >= 140 + i * 40 &&
					y < 174 + i * 40 && !hosts[i].started) {
					snprintf(address, capacity, "%s", hosts[i].address);
					choice = 1;
				}
			if (y >= 345 && y < 362)
				choice = 2;
			if (y >= 364 && y < 380)
				choice = 0;
		}
		if (!(frame++ % 5))
			DrawHostList(hosts, count, frame);
		SDL_Delay(16);
	}
	Network_StopDiscovery();
	return choice;
}

#define kClientPickerY 248

static void DrawClientLobby(const char *address, UInt64 waitedMS)
{
	int rowBytes, stride;
	char line[64];
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 112, 52, 416, 374, COL_DKGRAY);
	TR_FillRect(fb, stride, 116, 56, 408, 366, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 66,
		"JOIN LAN GAME", COL_YELLOW, 2);
	snprintf(line, sizeof(line), "HOST: %s", address);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 90, line, COL_WHITE, 1);
	if (Network_GetLocalSlot() > 0) {
		snprintf(line, sizeof(line), "CONNECTED AS DRIVER %d", Network_GetLocalSlot() + 1);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 104, line, COL_GREEN, 1);
		DrawLobbySettings(fb, stride, 120, 1);
		DrawLobbyDrivers(fb, stride, 180);
		DrawVehiclePicker(fb, stride, kClientPickerY);
		DrawNameField(fb, stride, 286);
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 306,
			"WAITING FOR THE HOST TO START THE RACE", COL_YELLOW, 1);
		DrawLanViewChoice(fb, stride, 326);
	} else {
		TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 194,
			"CONNECTING...", COL_YELLOW, 2);
		if (waitedMS > 5000) {
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 236,
				"NO ANSWER FROM THE HOST.", COL_RED, 1);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 254,
				"IS IT IN THE LAN LOBBY? DOES ITS FIREWALL", COL_WHITE, 1);
			TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 270,
				"ALLOW INCOMING CONNECTIONS FOR THE GAME?", COL_WHITE, 1);
		}
	}
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 400,
		"ESC  CANCEL", COL_GRAY, 1);
	Platform_Blit2Screen();
}

/* Waits after joining until the host starts the race (Network_Poll then
   starts the game). Returns 0 if the player cancelled. */
static int WaitInClientLobby(const char *address)
{
	UInt64 start = SDL_GetTicks64();
	int frame = 0;
	while (!gGameOn) {
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return 0;
		}
		{
			int x = -1, y = -1;
			int clicked = Platform_GetMouseClick(&x, &y);
			if (Network_GetLocalSlot() > 0 && HandleNameField(x, y, 286)) {
				/* typing: no lobby keys */
			} else {
				if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
					return 0;
				if (clicked && y >= 394 && y < 414)
					return 0;
				HandleVehiclePicker(x, y, kClientPickerY);
				if (Platform_WasKeyPressed(SDL_SCANCODE_V))
					Prefs_ToggleLanScreen();
			}
		}
		Network_Poll();
		if (gGameOn || gExit)
			break;
		if (!(frame++ % 5))
			DrawClientLobby(address, SDL_GetTicks64() - start);
		SDL_Delay(16);
	}
	return 1;
}

static void DrawAddressPrompt(const char *address)
{
	int rowBytes;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	int stride;
	if (!fb)
		return;
	stride = rowBytes/4;
	TR_FillRect(fb, stride, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BLACK);
	TR_FillRect(fb, stride, 112, 150, 416, 176, COL_DKGRAY);
	TR_FillRect(fb, stride, 116, 154, 408, 168, COL_BAR_BG);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 175,
		"JOIN LAN GAME", COL_YELLOW, 2);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 213,
		"ENTER HOST IPv4 ADDRESS", COL_WHITE, 1);
	TR_FillRect(fb, stride, 188, 238, 264, 30, COL_WHITE);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 249,
		address[0] ? address : "e.g. 192.168.1.10", COL_BLACK, 1);
	TR_DrawStringCentered(fb, stride, SCREEN_WIDTH / 2, 290,
		"ENTER  CONNECT     ESC  BACK", COL_GRAY, 1);
	Platform_Blit2Screen();
}

/* The host address typed last, kept in the preferences folder. */
static const char *LastHostPath(void)
{
	static char path[600];
	char *prefDir = SDL_GetPrefPath("DarrCoh", "RecklessDrivin");
	if (!prefDir)
		return NULL;
	snprintf(path, sizeof(path), "%sLastHost", prefDir);
	SDL_free(prefDir);
	return path;
}

static void LoadLastHost(char *address, size_t capacity)
{
	const char *path = LastHostPath();
	FILE *file = path ? fopen(path, "r") : NULL;
	size_t length;
	address[0] = '\0';
	if (!file)
		return;
	if (fgets(address, (int)capacity, file)) {
		/* only digits and dots, as typed */
		length = strspn(address, "0123456789.");
		address[length] = '\0';
	}
	fclose(file);
}

static void SaveLastHost(const char *address)
{
	const char *path = LastHostPath();
	FILE *file = path ? fopen(path, "w") : NULL;
	if (!file)
		return;
	fprintf(file, "%s\n", address);
	fclose(file);
}

static int EnterServerAddress(char *address, size_t capacity)
{
	static const SDL_Scancode digitKeys[] = {
		SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2,
		SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
		SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
		SDL_SCANCODE_9
	};
	size_t length;
	LoadLastHost(address, capacity);
	length = strlen(address);
	DrawAddressPrompt(address);
	for (;;) {
		int digit;
		Platform_PollEvents();
		if (Platform_ShouldQuit()) {
			gExit = true;
			return 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE))
			return 0;
		if (Platform_WasKeyPressed(SDL_SCANCODE_RETURN) ||
			Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER)) {
			if (length)
				SaveLastHost(address);
			return length != 0;
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_BACKSPACE) && length) {
			address[--length] = '\0';
			DrawAddressPrompt(address);
		}
		for (digit = 0; digit < 10; ++digit) {
			if (Platform_WasKeyPressed(digitKeys[digit]) &&
				length + 1 < capacity) {
				address[length++] = (char)('0' + digit);
				address[length] = '\0';
				DrawAddressPrompt(address);
			}
		}
		if (Platform_WasKeyPressed(SDL_SCANCODE_PERIOD) &&
			length + 1 < capacity) {
			address[length++] = '.';
			address[length] = '\0';
			DrawAddressPrompt(address);
		}
		SDL_Delay(16);
	}
}

/* The last LAN game: after a game over the host hosts it again and the
   others join the same host again, everyone back in the lobby. */
static int sLastHostPlayers;
static char sLastJoinAddress[16];

static void HostLanGame(int targetPlayers, int shiftHeld)
{
	int ready;
	if (!Network_InitHost(kMultiplayerPort)) {
		RestoreMenuScreen();
		return;
	}
	sLastHostPlayers = targetPlayers;
	Network_SetLobbyPlayerLimit(targetPlayers);
	ready = WaitForLanPlayers(targetPlayers);
	StopNameEdit();
	if (ready) {
		gStartLevel = gLevelGenOn ? 0 : Prefs_GetStartLevel();	/* generated: from its first level */
		StartGame(shiftHeld);
		gStartLevel = 0;
	} else {
		Network_Shutdown();
		RestoreMenuScreen();
	}
}

static void JoinLanGame(const char *address)
{
	int joined;
	if (!Network_InitClient(address, kMultiplayerPort)) {
		RestoreMenuScreen();
		return;
	}
	snprintf(sLastJoinAddress, sizeof(sLastJoinAddress), "%s", address);
	joined = WaitInClientLobby(address);
	StopNameEdit();
	if (!joined) {
		Network_Shutdown();
		RestoreMenuScreen();
	}
}

static void StartMultiplayer(int shiftHeld)
{
	int mode = ChooseMultiplayerMode();
	if (mode >= 0)
		sLastMultiplayerChoice = mode;
	if (mode < 0) {
		RestoreMenuScreen();
		return;
	}
	if (mode == 0) {
		int playerCount = gPrefs.localPlayerCount;
		WritePrefs(0);
		if (playerCount > 1)
			Network_EnableLocalPlayers(playerCount);
		Network_SetSoloModes(1);	/* until EndGame */
		gStartLevel = gLevelGenOn ? 0 : Prefs_GetStartLevel();	/* generated: from its first level */
		StartGame(shiftHeld);
		gStartLevel = 0;
		return;
	}
	if (mode == 1) {
		int targetPlayers = ChoosePlayerCount(2, "LAN PLAYER COUNT");
		if (!targetPlayers) {
			RestoreMenuScreen();
			return;
		}
		HostLanGame(targetPlayers, shiftHeld);
		return;
	}
	{
		char address[16];
		int choice = ChooseLanHost(address, sizeof(address));
		if (choice == 2)
			choice = EnterServerAddress(address, sizeof(address));
		if (!choice || gExit) {
			RestoreMenuScreen();
			return;
		}
		JoinLanGame(address);
	}
}

/* After a multiplayer game: the host hosts the same game again and waits in
   the lobby, the others rejoin the same host; a local game goes back to the
   multiplayer menu. */
static void ReturnToMultiplayer(void)
{
	if (sLastMultiplayerChoice == 1 && sLastHostPlayers)
		HostLanGame(sLastHostPlayers, 0);
	else if (sLastMultiplayerChoice == 2 && sLastJoinAddress[0])
		JoinLanGame(sLastJoinAddress);
	else
		StartMultiplayer(0);
}

/* ======================================================================== */
/* Mac PICT v2 renderer (minimal — handles DirectBitsRect for 16-bit)       */
/* ======================================================================== */

/* Read a big-endian UInt16 from unaligned memory */
static inline UInt16 RdBE16(const UInt8 *p)
{
	return (UInt16)((p[0] << 8) | p[1]);
}

/*
 * PackBits decompression for 16-bit pixel data (word-level).
 * Each unit is a 2-byte word.  Flag byte n:
 *   n >= 0:    copy (n+1) words literally  (2*(n+1) bytes)
 *   n < 0, != -128: repeat next word (1-n) times (2*(1-n) bytes)
 *   n == -128: no-op
 */
static void UnpackBitsRow16(const UInt8 *src, int srcLen, UInt8 *dst, int dstLen)
{
	const UInt8 *srcEnd = src + srcLen;
	UInt8 *dstEnd = dst + dstLen;

	while (src < srcEnd && dst + 1 < dstEnd) {
		int8_t n = (int8_t)*src++;
		if (n >= 0) {
			/* Literal run: copy (n+1) 16-bit words */
			int count = n + 1;
			int bytes = count * 2;
			for (int i = 0; i < bytes && src < srcEnd && dst < dstEnd; i++)
				*dst++ = *src++;
		} else if (n != (int8_t)-128) {
			/* Repeat run: repeat next 2 bytes (1-n) times */
			int count = 1 - (int)n;
			UInt8 b0 = (src < srcEnd) ? *src++ : 0;
			UInt8 b1 = (src < srcEnd) ? *src++ : 0;
			for (int i = 0; i < count && dst + 1 < dstEnd; i++) {
				*dst++ = b0;
				*dst++ = b1;
			}
		}
		/* n == -128: no-op */
	}
}

/*
 * Standard PackBits decompression (byte-level, for 8-bit data).
 */
static void UnpackBitsRow8(const UInt8 *src, int srcLen, UInt8 *dst, int dstLen)
{
	const UInt8 *srcEnd = src + srcLen;
	UInt8 *dstEnd = dst + dstLen;

	while (src < srcEnd && dst < dstEnd) {
		int8_t n = (int8_t)*src++;
		if (n >= 0) {
			int count = n + 1;
			while (count-- > 0 && src < srcEnd && dst < dstEnd)
				*dst++ = *src++;
		} else if (n != (int8_t)-128) {
			int count = 1 - (int)n;
			UInt8 val = (src < srcEnd) ? *src++ : 0;
			while (count-- > 0 && dst < dstEnd)
				*dst++ = val;
		}
	}
}

/*
 * Parse a Mac PICT v2 and render pixel data into the game's framebuffer.
 * Handles two opcodes:
 *   0x009A  DirectBitsRect — 16-bit 5-5-5 RGB pixels (most game screens)
 *   0x0098  PackBitsRect   — 8-bit indexed color with embedded CLUT (pause screen)
 */
static int RenderPICTToFramebuffer(const UInt8 *data, long dataSize)
{
	int rowBytes;
	tPixel *fb = Platform_GetFramebuffer(&rowBytes);
	if (!fb) return 0;

	/* Scan for DirectBitsRect (0x009A) or PackBitsRect (0x0098) */
	long pos = -1;
	int opcode = 0;
	for (long i = 10; i < dataSize - 1; i++) {
		if (data[i] == 0x00 && (data[i+1] == 0x9A || data[i+1] == 0x98)) {
			opcode = data[i+1];
			pos = i + 2; /* skip the opcode */
			break;
		}
	}
	if (pos < 0) return 0;

	/* DirectBitsRect has a 4-byte baseAddr prefix; PackBitsRect does not */
	if (opcode == 0x9A)
		pos += 4;

	/* Parse PixMap header */
	if (pos + 46 > dataSize) return 0;

	UInt16 pmRowBytesRaw = RdBE16(data + pos);
	int isPixMap = (pmRowBytesRaw & 0x8000) != 0;
	UInt16 pmRowBytes = pmRowBytesRaw & 0x3FFF;
	pos += 2;

	int boundsTop    = (int16_t)RdBE16(data + pos); pos += 2;
	int boundsLeft   = (int16_t)RdBE16(data + pos); pos += 2;
	int boundsBottom = (int16_t)RdBE16(data + pos); pos += 2;
	int boundsRight  = (int16_t)RdBE16(data + pos); pos += 2;

	int picWidth  = boundsRight - boundsLeft;
	int picHeight = boundsBottom - boundsTop;

	/* Read pixelSize from the PixMap header (we need it to decide 8-bit vs 16-bit) */
	/* PixMap fields after bounds: pmVersion(2) + packType(2) + packSize(4) +
	   hRes(4) + vRes(4) + pixelType(2) = 18 bytes to pixelSize */
	int pixelSize = 16; /* default for DirectBitsRect */
	if (isPixMap && pos + 20 <= dataSize) {
		pixelSize = (int)RdBE16(data + pos + 18);
	}

	/* Skip rest of PixMap header: 36 bytes total */
	pos += 36;

	/* For 8-bit indexed color: parse the embedded color table (CLUT) */
	tPixel clut[256];
	memset(clut, 0, sizeof(clut));

	if (pixelSize <= 8 && isPixMap) {
		/* Color table: ctSeed(4) + ctFlags(2) + ctSize(2) */
		if (pos + 8 > dataSize) return 0;
		/* UInt32 ctSeed = ... */ pos += 4;
		/* UInt16 ctFlags = ... */ pos += 2;
		UInt16 ctSize = RdBE16(data + pos); pos += 2;
		int numColors = (int)ctSize + 1;
		if (numColors > 256) numColors = 256;

		/* Each entry: value(2) + r(2) + g(2) + b(2) = 8 bytes */
		for (int c = 0; c < numColors; c++) {
			if (pos + 8 > dataSize) break;
			UInt16 idx = RdBE16(data + pos); pos += 2;
			UInt16 r   = RdBE16(data + pos); pos += 2;
			UInt16 g   = RdBE16(data + pos); pos += 2;
			UInt16 b   = RdBE16(data + pos); pos += 2;
			/* 16-bit Mac color channels, kept at 8 bits */
			tPixel pixel = PixelRGB(r >> 8, g >> 8, b >> 8);
			if (idx < 256)
				clut[idx] = pixel;
			else
				clut[c] = pixel; /* some PICTs use sequential indices */
		}
	}

	/* Skip srcRect(8) + dstRect(8) + mode(2) = 18 bytes */
	pos += 18;

	/* Now at the PackBits pixel data */
	if (picWidth <= 0 || picHeight <= 0 || picWidth > SCREEN_WIDTH || picHeight > SCREEN_HEIGHT)
		return 0;

	UInt8 rowBuf[SCREEN_WIDTH * 2]; /* max decompressed row */
	int fbRowPixels = rowBytes/4;

	/* pmRowBytes is an independent field (up to 0x3FFF); it bounds the
	 * memset and the UnpackBits writes into rowBuf, so reject any row that
	 * would not fit rather than overflow the stack buffer. */
	if (pmRowBytes == 0 || pmRowBytes > (int)sizeof(rowBuf))
		return 0;

	for (int y = 0; y < picHeight; y++) {
		if (pos >= dataSize) break;

		int packedLen;
		if (pmRowBytes > 250) {
			if (pos + 2 > dataSize) break;
			packedLen = (int)RdBE16(data + pos);
			pos += 2;
		} else {
			packedLen = data[pos];
			pos += 1;
		}

		if (pos + packedLen > dataSize) break;

		memset(rowBuf, 0, pmRowBytes);

		int pixelsThisRow = picWidth;
		if (pixelsThisRow > SCREEN_WIDTH) pixelsThisRow = SCREEN_WIDTH;
		int renderScale = Platform_GetRenderScale();

		if (pixelSize == 16) {
			/* 16-bit: word-level PackBits, then byte-swap each pixel */
			UnpackBitsRow16(data + pos, packedLen, rowBuf, pmRowBytes);
			for (int x = 0; x < pixelsThisRow; x++) {
				tPixel pixel = Pixel555((UInt16)((rowBuf[x*2] << 8) | rowBuf[x*2+1]));
				for (int sy = 0; sy < renderScale; sy++)
					for (int sx = 0; sx < renderScale; sx++)
						fb[(y * renderScale + sy) * fbRowPixels +
							x * renderScale + sx] = pixel;
			}
		} else {
			/* 8-bit: byte-level PackBits, then look up each index in CLUT */
			UnpackBitsRow8(data + pos, packedLen, rowBuf, pmRowBytes);
			for (int x = 0; x < pixelsThisRow; x++) {
				tPixel pixel = clut[rowBuf[x]];
				for (int sy = 0; sy < renderScale; sy++)
					for (int sx = 0; sx < renderScale; sx++)
						fb[(y * renderScale + sy) * fbRowPixels +
							x * renderScale + sx] = pixel;
			}
		}

		pos += packedLen;
	}

	return 1;
}

/* ======================================================================== */
/* ShowPicScreen — load, decompress, parse PICT, and display                 */
/* ======================================================================== */

void ShowPicScreen(int id)
{
	Handle pic;
	const tHQImage *hq;
	Platform_ScreenClear();

	hq=HQAssets_Load("pics",id,0);
	if(hq&&Platform_GetRenderScale()<=hq->scale&&
		hq->scale%Platform_GetRenderScale()==0)
	{
		int renderScale=Platform_GetRenderScale();
		int sourceStep=hq->scale/renderScale;
		int rowBytes;
		tPixel *fb = Platform_GetFramebuffer(&rowBytes);
		int width=hq->width*renderScale;
		int height=hq->height*renderScale;
		int fbStride=rowBytes/4;
		for(int y=0;y<height&&y<Platform_GetRenderHeight();y++)
		{
			tPixel *dst=fb+y*fbStride;
			int sourceY=y*sourceStep;
			for(int x=0;x<width&&x<Platform_GetRenderWidth();x++)
			{
				int sourceX=x*sourceStep;
				const UInt8 *src=hq->rgba+
					((size_t)sourceY*(hq->width*hq->scale)+sourceX)*4;
				if(src[3]>=128)
					dst[x]=PixelRGB(src[0],src[1],src[2]);
			}
		}
	}
	else
	{
		pic=Resources_Get(MakeFourCC("PPic"),id);
		if(pic)
		{
			long compressedSize=Resources_GetSize(pic);
			LZRWDecodeHandle(&pic,compressedSize);

			long decompSize = Resources_GetSize(pic);
			if(decompSize > 0)
				RenderPICTToFramebuffer((const UInt8*)*pic, decompSize);

			Resources_Release(pic);
		}
	}

	Platform_Blit2Screen();
	if(hq)
		HQAssets_Release("pics",id,0);
}

void ShowPicScreenNoFade(int id)
{
	ShowPicScreen(id);
}

void ScreenUpdate(void)
{
	Platform_Blit2Screen();
}

void DisposeInterface(void)
{
	if(gInterfaceInited)
	{
		gInterfaceInited=false;
	}
}

void SaveFlushEvents(void)
{
	Platform_FlushInput();
}

void InitInterface(void)
{
	if(!gInterfaceInited)
	{
		gInterfaceInited=true;
		LevelGen_LoadSettings();

		/* Load all three menu state images */
		LoadPPicToBuffer(1000, sMenuNormal);
		LoadPPicToBuffer(1001, sMenuHover);
		LoadPPicToBuffer(1002, sMenuPressed);
		DrawMenuLicensePlate(0, 0, 0);
		DrawMenuLicensePlate(1, 1, 0);
		DrawMenuLicensePlate(2, 1, 1);
		sMenuImagesLoaded = 1;
	}
	InputMode(kInputSuspended);
	/* Restore normal menu from buffer */
	if (sMenuImagesLoaded) {
		int rowBytes;
		tPixel *fb = Platform_GetFramebuffer(&rowBytes);
		memcpy(fb, sMenuNormal, (size_t)(rowBytes/4) *
			Platform_GetRenderHeight() * sizeof(tPixel));
		Platform_Blit2Screen();
	} else {
		ShowPicScreen(1000);
	}
	sHoverButton = kNoButton;
	/* Menu shortcuts are edge-triggered (KeyToButton), so keys still held
	 * from the game cannot fire immediately; flushing is enough. */
	SaveFlushEvents();
	gGameOn=false;
}

void WaitForPress(void)
{
	int pressed=false;
	int prevContinue;
	Platform_ShowCursor();
	/* Consume anything still pending from the previous screen; keys held
	 * across the transition then need a fresh press to register. */
	Platform_FlushInput();
	prevContinue=Platform_ContinuePress();
	while(!pressed)
	{
		Platform_PollEvents();
		if(Platform_ShouldQuit())
			break;
		if(Platform_WasKeyPressed(SDL_SCANCODE_RETURN)||
		   Platform_WasKeyPressed(SDL_SCANCODE_KP_ENTER)||
		   Platform_WasKeyPressed(SDL_SCANCODE_SPACE)||
		   Platform_WasKeyPressed(SDL_SCANCODE_ESCAPE)||
		   Platform_GetMouseClick(NULL,NULL))
			pressed=true;
		/* Gamepad action buttons are level-based; take the rising edge. */
		{
			int cont=Platform_ContinuePress();
			if(cont&&!prevContinue)
				pressed=true;
			prevContinue=cont;
		}
		SDL_Delay(16);
	}
	SaveFlushEvents();
}

/* Restore the normal menu screen from buffer (avoids re-parsing PICT) */
static void RestoreMenuScreen(void)
{
	if (sMenuImagesLoaded) {
		int rowBytes;
		tPixel *fb = Platform_GetFramebuffer(&rowBytes);
		memcpy(fb, sMenuNormal, (size_t)(rowBytes/4) *
			Platform_GetRenderHeight() * sizeof(tPixel));
		Platform_Blit2Screen();
	} else {
		ShowPicScreen(1000);
	}
}

/* Holding Shift while starting the game opens the level/vehicle select
 * (cheat mode), whether the start came from Enter or a mouse click. */
static int ShiftHeld(void)
{
	return Platform_IsKeyDown(SDL_SCANCODE_LSHIFT) ||
	       Platform_IsKeyDown(SDL_SCANCODE_RSHIFT);
}

static void HandleCommand(int cmd, int shiftHeld)
{
	switch(cmd)
	{
		case kNoButton: return;
		case kRegisterButton:
			Register(true);
			RestoreMenuScreen();
			break;
		case kStartGameButton:
			StartGame(shiftHeld);
			break;
		case kMultiplayerButton:
			StartMultiplayer(shiftHeld);
			break;
		case kPrefsButton:
			Preferences();
			RestoreMenuScreen();
			break;
		case kHelpButton:
			ShowPicScreen(1007);
			WaitForPress();
			ShowPicScreen(1008);
			WaitForPress();
			RestoreMenuScreen();
			break;
		case kScoreButton:
			ShowHighScores(-1);
			RestoreMenuScreen();
			break;
		case kQuitButton:
			gExit=true;
			break;
	}
}

/* Show press feedback for a button, then execute the command */
static void ActivateButton(int button)
{
	/* Sample Shift at the moment of activation so the level select cheat
	 * triggers even if the chord is released during the press animation. */
	int shiftHeld = ShiftHeld();
	if (sMenuImagesLoaded && button >= 0 && button < (int)kNumButtons) {
		BlitButtonRect(button, sMenuPressed);
		Platform_Blit2Screen();
		SimplePlaySound(147);
		SDL_Delay(100);
		/* Re-read after the delay as a grace period for a rolled Shift+Enter
		 * chord where Enter arrived a frame before Shift. */
		Platform_PollEvents();
		shiftHeld = shiftHeld || ShiftHeld();
	}
	HandleCommand(button, shiftHeld);
	/* Restore menu after returning from the command */
	if (!gGameOn && !gExit) {
		sHoverButton = kNoButton;
	}
}

void Eventloop(void)
{
	Platform_PollEvents();
	Platform_ShowCursor();
	/* After a multiplayer game: straight back to it, not the main menu. */
	if (sReturnToMultiplayer && !gExit) {
		sReturnToMultiplayer = 0;
		ReturnToMultiplayer();
		return;
	}

	if(Platform_ShouldQuit())
	{
		gExit=true;
		return;
	}

	/* --- Mouse hover tracking --- */
	if (sMenuImagesLoaded)
	{
		int mx, my, down;
		Platform_GetMouseState(&mx, &my, &down);
		int hover = HitTestButtons(mx, my);

		if (hover != sHoverButton) {
			/* Restore previous button to normal */
			if (sHoverButton != kNoButton)
				BlitButtonRect(sHoverButton, sMenuNormal);
			/* Show hover state for new button */
			if (hover != kNoButton)
				BlitButtonRect(hover, sMenuHover);
			sHoverButton = hover;
			Platform_Blit2Screen();
		}
	}

	/* --- Mouse click on menu buttons --- */
	{
		int mx, my;
		if(Platform_GetMouseClick(&mx, &my))
		{
			int button = HitTestButtons(mx, my);
			if(button != kNoButton)
			{
				ActivateButton(button);
				return;
			}
		}
	}

	/* --- Keyboard shortcuts with visual feedback --- */
	{
		if (Platform_WasKeyPressed(SDL_SCANCODE_2) && !Network_IsActive()) {
			Network_EnableLocalTwoPlayer();
			ActivateButton(kStartGameButton);
			return;
		}
		int button = KeyToButton();
		if (button != kNoButton) {
			ActivateButton(button);
			return;
		}
	}
}
