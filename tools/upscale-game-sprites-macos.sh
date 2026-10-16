#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
TOOL_DIR="$BUILD_DIR/ai-upscaler"
UPSCALER="$TOOL_DIR/realesrgan-ncnn-vulkan"
ARCHIVE="$TOOL_DIR/realesrgan-ncnn-vulkan-macos.zip"
RELEASE_URL="https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-macos.zip"
ASSET_ROOT="$BUILD_DIR/AIAssets"
# ONLY_SPRITES=1 regenerates just the car and scenery sprites.
ONLY_SPRITES="${ONLY_SPRITES:-0}"
# ONLY_SCENE=1 regenerates just textures, HUD, menu pictures and the font.
ONLY_SCENE="${ONLY_SCENE:-0}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This experimental asset script currently supports macOS only." >&2
    exit 1
fi
for command in cmake codesign curl ffmpeg file unzip; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

mkdir -p "$TOOL_DIR"
if [[ ! -x "$UPSCALER" || ! -f "$TOOL_DIR/models/realesrgan-x4plus-anime.bin" ||
      ! -f "$TOOL_DIR/models/realesrgan-x4plus-anime.param" ||
      ! -f "$TOOL_DIR/models/realesrgan-x4plus.bin" ||
      ! -f "$TOOL_DIR/models/realesrgan-x4plus.param" ]]; then
    curl -fL --retry 2 -o "$ARCHIVE" "$RELEASE_URL"
    unzip -Z1 "$ARCHIVE" | grep -qx 'realesrgan-ncnn-vulkan'
    unzip -o "$ARCHIVE" 'realesrgan-ncnn-vulkan' 'models/*' -d "$TOOL_DIR" >/dev/null
    chmod u+x "$UPSCALER"
fi
if ! file "$UPSCALER" | grep -q 'Mach-O'; then
    echo "Downloaded upscaler is not a macOS executable." >&2
    exit 1
fi

cmake -S "$ROOT" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target ai_sprite_assets hq_scene_assets RecklessDrivin -j4
if [[ -d "$BUILD_DIR/RecklessDrivin.app" ]]; then
    ditto "$BUILD_DIR/RecklessDrivin.app" "$BUILD_DIR/Reckless Drivin'.app"
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/reckless-ai-sprites.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
SOURCE_DIR="$WORK_DIR/source"
INPUT_DIR="$WORK_DIR/input"
UPSCALED_DIR="$WORK_DIR/upscaled"
RAW_DIR="$WORK_DIR/raw"
mkdir -p "$SOURCE_DIR" "$INPUT_DIR/sprites" "$UPSCALED_DIR/sprites" \
    "$RAW_DIR/sprites" "$ASSET_ROOT/sprites"
for category in textures rle pics chars; do
    mkdir -p "$SOURCE_DIR/$category" "$INPUT_DIR/$category" \
        "$UPSCALED_DIR/$category" "$RAW_DIR/$category" "$ASSET_ROOT/$category"
done

if [[ "$ONLY_SCENE" != 1 ]]; then
"$BUILD_DIR/ai_sprite_assets" export "$ROOT/Data" "$SOURCE_DIR"
fi
if [[ "$ONLY_SPRITES" != 1 ]]; then
    "$BUILD_DIR/hq_scene_assets" export "$ROOT/Data" "$SOURCE_DIR"
fi

shopt -s nullglob
if [[ "$ONLY_SCENE" != 1 ]]; then
for ppm in "$SOURCE_DIR/"*.ppm; do
    sprite_id="$(basename "$ppm" .ppm)"
    ffmpeg -v error -y -i "$ppm" -i "$SOURCE_DIR/$sprite_id.pgm" \
        -filter_complex '[0:v][1:v]alphamerge' -frames:v 1 \
        "$INPUT_DIR/sprites/$sprite_id.png"
done
"$UPSCALER" -i "$INPUT_DIR/sprites" -o "$UPSCALED_DIR/sprites" \
    -m "$TOOL_DIR/models" -n realesrgan-x4plus-anime -s 4

# The sprite mask is scaled smoothly: the game stores sprites with 8-bit
# alpha and blends their edges instead of cutting 4x4 pixel steps.
for mask in "$SOURCE_DIR/"*.pgm; do
    sprite_id="$(basename "$mask" .pgm)"
    ffmpeg -v error -y -i "$UPSCALED_DIR/sprites/$sprite_id.png" -i "$mask" \
        -filter_complex '[1:v]scale=iw*4:ih*4:flags=bicubic[mask];[0:v][mask]alphamerge,format=rgba' \
        -frames:v 1 -f rawvideo -pix_fmt rgba "$RAW_DIR/sprites/$sprite_id.rgba"
done

"$BUILD_DIR/ai_sprite_assets" pack "$ROOT/Data" "$RAW_DIR/sprites" \
    "$ASSET_ROOT/sprites"
fi

scene_categories="textures rle pics chars"
if [[ "$ONLY_SPRITES" == 1 ]]; then
    scene_categories=""
fi
for category in $scene_categories; do
    for ppm in "$SOURCE_DIR/$category/"*.ppm; do
        name="$(basename "$ppm" .ppm)"
        ffmpeg -v error -y -i "$ppm" -i "$SOURCE_DIR/$category/$name.pgm" \
            -filter_complex '[0:v][1:v]alphamerge' -frames:v 1 \
            "$INPUT_DIR/$category/$name.png"
    done
    if [[ "$category" == rle || "$category" == chars ]]; then
        model=realesrgan-x4plus-anime
    else
        model=realesrgan-x4plus
    fi
    if [[ "$category" == textures ]]; then
        # Road surfaces, borders, skid tracks and marks: x4plus draws vertical
        # stripes into them, animevideov3 keeps them clean. Grass, water and
        # other backgrounds stay on x4plus.
        mkdir -p "$INPUT_DIR/textures-road"
        for png in "$INPUT_DIR/textures/"*.png; do
            texture_id="$(basename "$png" .png)"
            texture_id="${texture_id%%_*}"
            case "$texture_id" in
                129|130|135|137|10[0-9][0-9]|20[0-9][0-9]|30[0-9][0-9])
                    mv "$png" "$INPUT_DIR/textures-road/" ;;
            esac
        done
        "$UPSCALER" -i "$INPUT_DIR/textures-road" -o "$UPSCALED_DIR/textures" \
            -m "$TOOL_DIR/models" -n realesr-animevideov3-x4 -s 4
    fi
    "$UPSCALER" -i "$INPUT_DIR/$category" -o "$UPSCALED_DIR/$category" \
        -m "$TOOL_DIR/models" -n "$model" -s 4

    for mask in "$SOURCE_DIR/$category/"*.pgm; do
        name="$(basename "$mask" .pgm)"
        if [[ "$category" == textures ]]; then
            ffmpeg -v error -y -i "$UPSCALED_DIR/$category/$name.png" -i "$mask" \
                -filter_complex '[0:v]crop=iw/3:ih/3:iw/3:ih/3[detail];[1:v]crop=iw/3:ih/3:iw/3:ih/3,scale=iw*4:ih*4:flags=neighbor[mask];[detail][mask]alphamerge,format=rgba' \
                -frames:v 1 -f rawvideo -pix_fmt rgba "$RAW_DIR/$category/$name.rgba"
        elif [[ "$category" == chars ]]; then
            ffmpeg -v error -y -i "$UPSCALED_DIR/$category/$name.png" -i "$mask" \
                -filter_complex '[1:v]scale=iw*4:ih*4:flags=bicubic[mask];[0:v][mask]alphamerge,format=rgba' \
                -frames:v 1 -f rawvideo -pix_fmt rgba "$RAW_DIR/$category/$name.rgba"
        else
            ffmpeg -v error -y -i "$UPSCALED_DIR/$category/$name.png" -i "$mask" \
                -filter_complex '[1:v]scale=iw*4:ih*4:flags=neighbor[mask];[0:v][mask]alphamerge,format=rgba' \
                -frames:v 1 -f rawvideo -pix_fmt rgba "$RAW_DIR/$category/$name.rgba"
        fi
    done
done

if [[ "$ONLY_SPRITES" != 1 ]]; then
    "$BUILD_DIR/hq_scene_assets" pack "$SOURCE_DIR" "$RAW_DIR" "$ASSET_ROOT"
fi

# Lossless zlib compression: the raw RGBA images take over 300 MB.
python3 "$(dirname "$0")/compress-ai-assets.py" "$ASSET_ROOT"

for app in "$BUILD_DIR/RecklessDrivin.app" "$BUILD_DIR/Reckless Drivin'.app"; do
    if [[ -d "$app/Contents/Resources" ]]; then
        mkdir -p "$app/Contents/Resources/AIAssets"
        for category in sprites textures rle pics chars; do
            mkdir -p "$app/Contents/Resources/AIAssets/$category"
            cp -R "$ASSET_ROOT/$category/." \
                "$app/Contents/Resources/AIAssets/$category/"
        done
        codesign --force --deep --sign - --timestamp=none "$app"
    fi
done

echo "HQ sprites, road textures, HUD images, and menu pictures are ready."
echo "Open the app, enable 'HQ 4K Mode' and 'AI Detail' in Preferences, then restart."
