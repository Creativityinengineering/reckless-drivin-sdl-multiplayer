/*
 * sound.c - Sound system for Reckless Drivin' SDL port
 *
 * Sound data is stored in pack files as tSound structures:
 *   { UInt32 numSamples, UInt32 priority, UInt32 flags, UInt32 offsets[] }
 * Each offset points to a Mac SoundHeader (stdSH, encode=0x00, 22 bytes)
 * or ExtSoundHeader (extSH, encode=0xFF, 64 bytes) followed by PCM data.
 * All multi-byte fields are big-endian.
 */

#include "compat.h"
#include "platform.h"
#include "objects.h"
#include "roads.h"
#include "preferences.h"
#include "packs.h"
#include "random.h"
#include "vec2d.h"
#include "netgame.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>

/* ======================================================================== */
/* Constants (from original)                                                 */
/* ======================================================================== */

#define kNumSoundChannels kNumSFXChannels
#define kMaxPanDist       400.0f
#define kMaxListenDist    1250.0f
#define kDopplerFactor    0.004f
#define kMaxNoiseVelo     65.0f
#define kMinSqueakSlide   0.5f
#define kSqueakFactor     0.5f
#define kNumGears         4
#define kHighestGear      55.0f
#define kHighestGearTurbo 70.0f
#define kShiftTolerance   2.5f

enum {
    kSoundPriorityHigher = 1 << 0
};

/* ======================================================================== */
/* Sound pack header structure                                               */
/* ======================================================================== */

typedef struct {
    UInt32 numSamples;
    UInt32 priority;
    UInt32 flags;
    UInt32 offsets[1];   /* variable-length array; numSamples entries */
} tSound;

/* ======================================================================== */
/* Mac SoundHeader constants                                                 */
/* ======================================================================== */

#define kStdSH_HeaderSize 22   /* Standard SoundHeader (encode=0x00) */
#define kExtSH_HeaderSize 64   /* Extended SoundHeader (encode=0xFF) */

/* ======================================================================== */
/* Globals                                                                   */
/* ======================================================================== */

float gVolume;
int   gGear;

static UInt32 sChannelPriority[kNumSoundChannels];
static int sAudioInited = 0;
/* Per driver on this computer: own engine and skid loops and own gears. */
static int sEngineNeedsRetrigger[kAudioCarChannels] = {1, 1, 1, 1};
static int sSkidNeedsRetrigger[kAudioCarChannels]   = {1, 1, 1, 1};
static int sGear[kAudioCarChannels];

static void RetriggerAllCars(void)
{
    int car;
    for (car = 0; car < kAudioCarChannels; car++) {
        sEngineNeedsRetrigger[car] = 1;
        sSkidNeedsRetrigger[car] = 1;
    }
}

/* How many drivers play on this computer (split screen) and which one is
   bound now. */
static int CarSoundCount(void)
{
    int count = Network_IsLocalMultiplayer() ? Network_GetLocalDriverCount() : 1;
    return count < 1 ? 1 : (count > kAudioCarChannels ? kAudioCarChannels : count);
}

static int CarSoundIndex(void)
{
    int slot = Network_GetLocalDriverSlot(gPlayerObj);
    return slot >= 0 && slot < CarSoundCount() ? slot : 0;
}

/* Split screen: a driver's own sounds come from his side of the screen
   (views are laid out in two columns). */
static float ViewPan(int car)
{
    if (CarSoundCount() < 2)
        return 0.0f;
    return car % 2 ? 0.55f : -0.55f;
}

static void SetPannedVolume(int channel, float volume, float pan)
{
    Platform_SetChannelVolume(channel, volume * (pan > 0 ? 1.0f - pan : 1.0f),
        volume * (pan < 0 ? 1.0f + pan : 1.0f));
}


/* ======================================================================== */
/* Helpers: read big-endian tSound fields                                    */
/* ======================================================================== */

static inline UInt32 SoundNumSamples(const tSound *s)
{
    return SWAP32(s->numSamples);
}

static inline UInt32 SoundPriority(const tSound *s)
{
    return SWAP32(s->priority);
}

static inline UInt32 SoundFlags(const tSound *s)
{
    return SWAP32(s->flags);
}

static inline UInt32 SoundOffset(const tSound *s, int index)
{
    return SWAP32(s->offsets[index]);
}

/* Read a big-endian UInt32 from unaligned memory */
static inline UInt32 ReadBE32(const UInt8 *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
           ((UInt32)p[2] << 8)  | (UInt32)p[3];
}

/* ======================================================================== */
/* SoundHeader parsing                                                       */
/* ======================================================================== */

/*
 * Parse a Mac SoundHeader at the given offset within a tSound entry.
 * Returns a pointer to the actual PCM sample data, the number of sample
 * frames, the native sample rate in Hz, and the bits per sample.
 *
 * Standard SoundHeader (encode=0x00, 22 bytes):
 *   Offset  Size  Field
 *   0       4     samplePtr        (always 0 for inline data)
 *   4       4     numSampleFrames
 *   8       4     sampleRate       (Fixed 16.16, in Hz)
 *   12      4     loopStart
 *   16      4     loopEnd
 *   20      1     encode           (0x00)
 *   21      1     baseFrequency
 *   22      ...   sampleArea       (unsigned 8-bit PCM)
 *
 * Extended SoundHeader (encode=0xFF, 64 bytes):
 *   Offset  Size  Field
 *   0       4     samplePtr        (always 0 for inline data)
 *   4       4     numChannels
 *   8       4     sampleRate       (Fixed 16.16, in Hz)
 *   12      4     loopStart
 *   16      4     loopEnd
 *   20      1     encode           (0xFF)
 *   21      1     baseFrequency
 *   22      4     numSampleFrames
 *   26      10    AIFFSampleRate   (80-bit extended)
 *   36      4     markerChunk
 *   40      4     instrumentChunks
 *   44      4     AESRecording
 *   48      2     sampleSize       (bits per sample)
 *   50      14    futureUse
 *   64      ...   sampleArea       (signed 16-bit big-endian PCM)
 */
static const UInt8 *ParseSoundHeader(const UInt8 *data, int dataLen,
                                     int *outNumFrames, float *outRate,
                                     int *outBitsPerSample)
{
    if (dataLen < kStdSH_HeaderSize) {
        *outNumFrames = 0;
        *outRate = 22050.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    UInt8 encode = data[20];

    if (encode == 0xFF) {
        /* Extended SoundHeader */
        if (dataLen < kExtSH_HeaderSize) {
            *outNumFrames = 0;
            *outRate = 22050.0f;
            *outBitsPerSample = 16;
            return NULL;
        }

        UInt32 sampleRateFixed = ReadBE32(data + 8);
        UInt32 numFrames = ReadBE32(data + 22);
        UInt16 sampleSize = ((UInt16)data[48] << 8) | data[49];

        *outRate = sampleRateFixed / 65536.0f;
        *outNumFrames = (int)numFrames;
        *outBitsPerSample = (sampleSize > 0) ? (int)sampleSize : 16;

        return data + kExtSH_HeaderSize;
    } else {
        /* Standard SoundHeader (encode=0x00) */
        UInt32 sampleRateFixed = ReadBE32(data + 8);
        UInt32 numFrames = ReadBE32(data + 4);

        *outRate = sampleRateFixed / 65536.0f;
        *outNumFrames = (int)numFrames;
        *outBitsPerSample = 8;

        return data + kStdSH_HeaderSize;
    }
}

/*
 * Get sample data from a tSound entry.  Parses the Mac SoundHeader at
 * the sample's offset to extract the actual PCM data pointer, frame
 * count, sample rate, and bit depth.  The pitch is adjusted to account
 * for the native sample rate vs. the mixer output rate.
 */
static const UInt8 *GetSampleData(const tSound *s, int sampleIndex,
                                  int totalEntrySize,
                                  int *outLen, float *outPitchAdj,
                                  int *outBitsPerSample)
{
    UInt32 numSamples = SoundNumSamples(s);
    if (sampleIndex < 0 || (UInt32)sampleIndex >= numSamples) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    /* numSamples is itself an untrusted field, so the offsets[] table it
     * sizes may extend past the entry. Verify the table fits before indexing
     * it, otherwise SoundOffset reads out of bounds. */
    if ((long long)offsetof(tSound, offsets) + (long long)numSamples * sizeof(UInt32)
            > (long long)totalEntrySize) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    UInt32 offset = SoundOffset(s, sampleIndex);
    /* The offset is an untrusted resource field; reject it if it does not
     * land inside the entry, otherwise hdrStart points past the buffer. */
    if (offset >= (UInt32)totalEntrySize) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }
    const UInt8 *base = (const UInt8 *)s;
    const UInt8 *hdrStart = base + offset;

    /* Compute raw data length at this offset */
    int rawLen;
    if ((UInt32)sampleIndex + 1 < numSamples) {
        UInt32 nextOffset = SoundOffset(s, sampleIndex + 1);
        rawLen = (int)(nextOffset - offset);
    } else {
        rawLen = totalEntrySize - (int)offset;
    }
    /* Clamp to the bytes actually present; a bogus next offset must not let
     * rawLen exceed the entry. */
    if (rawLen > totalEntrySize - (int)offset)
        rawLen = totalEntrySize - (int)offset;

    if (rawLen <= 0) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    int numFrames;
    float nativeRate;
    int bitsPerSample;
    const UInt8 *pcmData = ParseSoundHeader(hdrStart, rawLen,
                                            &numFrames, &nativeRate,
                                            &bitsPerSample);
    if (!pcmData || numFrames <= 0) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    /* Bound the header-declared frame count by the PCM bytes that actually
     * follow the header, so the mixer thread cannot read past the entry. */
    int headerSize = (int)(pcmData - hdrStart);
    int bytesPerFrame = (bitsPerSample >= 16) ? 2 : 1;
    int availFrames = (rawLen - headerSize) / bytesPerFrame;
    if (numFrames > availFrames)
        numFrames = availFrames;
    if (numFrames <= 0) {
        *outLen = 0;
        *outPitchAdj = 1.0f;
        *outBitsPerSample = 8;
        return NULL;
    }

    *outLen = numFrames;
    *outBitsPerSample = bitsPerSample;
    /* Adjust pitch so the mixer plays back at the correct native rate */
    *outPitchAdj = nativeRate / (float)AUDIO_MIX_RATE;

    return pcmData;
}

/* ======================================================================== */
/* Volume                                                                    */
/* ======================================================================== */

void SetGameVolume(int volume)
{
    if (volume == -1)
        gVolume = gPrefs.volume / 256.0f;
    else
        gVolume = volume / 256.0f;
}

void SetSystemVolume(void)
{
    /* no-op in SDL port */
}

/* ======================================================================== */
/* Channel init / shutdown                                                   */
/* ======================================================================== */

void InitChannels(void)
{
    if (!sAudioInited) {
        Platform_InitAudio();
        sAudioInited = 1;
    }

    for (int i = 0; i < kNumSoundChannels; i++)
        sChannelPriority[i] = 0;

    RetriggerAllCars();
}

void BeQuiet(void)
{
    for (int i = 0; i < kNumSoundChannels; i++) {
        Platform_StopChannel(kAudioChannel0 + i);
        sChannelPriority[i] = 0;
    }
    for (int ch = kAudioChannelEngine; ch < kNumAudioChannels; ch++)
        Platform_StopChannel(ch);

    RetriggerAllCars();
}

/* ======================================================================== */
/* Pick a free (lowest-priority) channel                                     */
/* ======================================================================== */

static int PickChannel(UInt32 *outPriority)
{
    int numChan = kNumSoundChannels;
    int bestChan = 0;
    UInt32 bestPri = 0xFFFFFFFF;

    for (int i = 0; i < numChan; i++) {
        /* Clear stale priority if the channel has finished playing */
        if (sChannelPriority[i] && !Platform_IsChannelActive(kAudioChannel0 + i))
            sChannelPriority[i] = 0;

        if (sChannelPriority[i] < bestPri) {
            bestPri = sChannelPriority[i];
            bestChan = i;
        }
    }
    if (outPriority)
        *outPriority = bestPri;
    return bestChan;
}

/* ======================================================================== */
/* Engine / skid continuous sound                                            */
/* ======================================================================== */

static void StartLoop(int channel, int soundId)
{
    int entrySize;
    tSound *sound = (tSound *)GetSortedPackEntry(kPackSnds, soundId, &entrySize);
    if (sound) {
        int sampleLen, bps;
        float pitchAdj;
        UInt32 ns = SoundNumSamples(sound);
        int idx = (ns > 0) ? RanInt(0, ns) : 0;
        const UInt8 *sampleData = GetSampleData(sound, idx, entrySize,
                                                &sampleLen, &pitchAdj, &bps);
        if (sampleData && sampleLen > 0)
            Platform_PlaySound(channel, sampleData, sampleLen, 0.0f, 0.0f, pitchAdj, bps);
    }
}

void SetCarSound(float engine, float skidL, float skidR, float velo)
{
    if (!gPrefs.sound)
        return;
    if (!gPrefs.engineSound && !gPrefs.skidSound)
        return;

    /* The bound driver's own loops: in split screen every driver has one. */
    int car = CarSoundIndex();
    int engineChannel = kAudioChannelEngine + 2 * car;
    int skidChannel = kAudioChannelSkid + 2 * car;
    float pan = ViewPan(car);
    /* Several engines at once: each a little quieter. */
    float mix = CarSoundCount() > 1 ? 0.7f : 1.0f;
    float engineVol, gearVelo;

    if (!(*gRoadInfo).water) {
        float highestGear = (gCurPlayer->addOns & kAddOnTurbo)
                            ? kHighestGearTurbo : kHighestGear;
        while (velo > (sGear[car] + 1) * highestGear / kNumGears + kShiftTolerance
               && (sGear[car] + 1) < kNumGears)
            sGear[car]++;
        while (velo < sGear[car] * highestGear / kNumGears - kShiftTolerance
               && sGear[car] > 0)
            sGear[car]--;

        gearVelo = fabsf(velo) / highestGear * kNumGears - sGear[car];
        velo /= kMaxNoiseVelo;
        if (gearVelo > 2) gearVelo = 2;
    } else {
        velo /= kMaxNoiseVelo;
        gearVelo = (engine + velo) / 2;
    }
    gGear = sGear[0];

    velo = fabsf(velo);

    if (engine != -1)
        engineVol = -gVolume / ((0.6f * engine + 0.15f * velo + 0.25f * gearVelo) - 2);
    else
        engineVol = 0;

    /* Re-trigger engine loop if needed */
    if (gPrefs.engineSound && sEngineNeedsRetrigger[car]) {
        StartLoop(engineChannel, 132);
        sEngineNeedsRetrigger[car] = 0;
    }

    /* Set engine volume and pitch */
    if (gPrefs.engineSound) {
        SetPannedVolume(engineChannel, 0.28f * engineVol * gVolume * mix, pan);
        float pitch = 0.2f + 0.3f * engine + 0.2f * velo + 0.3f * gearVelo;
        Platform_SetChannelPitch(engineChannel, pitch);
    }

    if (!gPrefs.skidSound)
        return;

    /* Skid processing */
    skidL -= kMinSqueakSlide;
    skidL /= kSqueakFactor;
    if (skidL < 0) skidL = 0;
    else if (skidL > 1) skidL = 1;

    skidR -= kMinSqueakSlide;
    skidR /= kSqueakFactor;
    if (skidR < 0) skidR = 0;
    else if (skidR > 1) skidR = 1;

    /* Re-trigger skid loop if needed */
    if (sSkidNeedsRetrigger[car]) {
        StartLoop(skidChannel, (*gRoadInfo).skidSound);
        sSkidNeedsRetrigger[car] = 0;
    }

    /* Set skid volume and pitch: the car's own left/right wheels, moved to
       the driver's side of the screen in split screen. */
    {
        float volL = skidL * gVolume * (velo * 0.5f + 0.5f) * mix;
        float volR = skidR * gVolume * (velo * 0.5f + 0.5f) * mix;
        Platform_SetChannelVolume(skidChannel,
            volL * (pan > 0 ? 1.0f - pan : 1.0f) + (pan < 0 ? volR * -pan : 0.0f),
            volR * (pan < 0 ? 1.0f + pan : 1.0f) + (pan > 0 ? volL * pan : 0.0f));

        float skidPitch;
        if (skidL + skidR)
            skidPitch = -1.0f / ((skidL + skidR) * 0.25f + velo * 0.5f - 2.0f);
        else
            skidPitch = 0.0f;
        Platform_SetChannelPitch(skidChannel, skidPitch);
    }
}

/* ======================================================================== */
/* Start the engine and skid loops                                           */
/* ======================================================================== */

void StartCarChannels(void)
{
    int car;
    for (int ch = kAudioChannelEngine; ch < kNumAudioChannels; ch++)
        Platform_StopChannel(ch);
    RetriggerAllCars();

    /* One engine and one skid loop for every driver on this computer. */
    for (car = 0; car < CarSoundCount(); car++) {
        if (gPrefs.sound && gPrefs.engineSound) {
            StartLoop(kAudioChannelEngine + 2 * car, 132);
            sEngineNeedsRetrigger[car] = 0;
        }
        if (gPrefs.sound && gPrefs.skidSound) {
            StartLoop(kAudioChannelSkid + 2 * car, (*gRoadInfo).skidSound);
            sSkidNeedsRetrigger[car] = 0;
        }
        Platform_SetChannelVolume(kAudioChannelEngine + 2 * car, 0, 0);
        Platform_SetChannelVolume(kAudioChannelSkid + 2 * car, 0, 0);
    }
}

/* The ears for a sound in the world: in split screen the driver closest to
   it, whose side of the screen it then comes from. */
static const tObject *Listener(t2DPoint pos, float *viewPan)
{
    const tObject *best = gCameraObj;
    float bestDistance = 1e30f;
    int car, count = CarSoundCount();
    *viewPan = 0.0f;
    if (count < 2)
        return gCameraObj;
    for (car = 0; car < count; car++) {
        const tObject *driver = Network_GetLocalDriverObject(car);
        float distance;
        if (!driver)
            continue;
        distance = VEC2D_Value(VEC2D_Difference(pos, driver->pos));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = driver;
            *viewPan = ViewPan(car);
        }
    }
    return best;
}

/* ======================================================================== */
/* Positional sound with distance attenuation and Doppler effect             */
/* ======================================================================== */

void PlaySound(t2DPoint pos, t2DPoint velo, float freq, float vol, int id)
{
    if (!gPrefs.sound)
        return;

    int entrySize;
    tSound *sound = (tSound *)GetSortedPackEntry(kPackSnds, id, &entrySize);
    if (!sound)
        return;

    UInt32 soundPri = SoundPriority(sound);
    UInt32 soundFlags = SoundFlags(sound);

    UInt32 chanPriority;
    int chanIdx = PickChannel(&chanPriority);

    if (soundFlags & kSoundPriorityHigher) {
        if (chanPriority >= soundPri)
            return;
    } else {
        if (chanPriority > soundPri)
            return;
    }

    float viewPan;
    const tObject *listener = Listener(pos, &viewPan);
    float dist = 1.0f - VEC2D_Value(VEC2D_Difference(pos, listener->pos))
                        * (1.0f / kMaxListenDist);
    if (dist <= 0)
        return;

    sChannelPriority[chanIdx] = soundPri;

    float pan = (pos.x - listener->pos.x) * (1.0f / kMaxPanDist);
    if (viewPan)
        pan = viewPan + pan * 0.45f;	/* split screen: from that view's side */
    if (pan > 1.0f)  pan = 1.0f;
    if (pan < -1.0f) pan = -1.0f;

    if (gPrefs.hqSound) {
        t2DPoint veloDiff = VEC2D_Difference(velo, listener->velo);
        t2DPoint spaceDiff = VEC2D_Norm(VEC2D_Difference(listener->pos, pos));
        freq *= powf(2.0f, VEC2D_DotProduct(veloDiff, spaceDiff) * kDopplerFactor);
    }

    vol *= gVolume;

    UInt32 ns = SoundNumSamples(sound);
    int sampleIdx = (ns > 0) ? RanInt(0, ns) : 0;
    int sampleLen, bps;
    float pitchAdj;
    const UInt8 *sampleData = GetSampleData(sound, sampleIdx, entrySize,
                                            &sampleLen, &pitchAdj, &bps);
    if (!sampleData || sampleLen <= 0)
        return;

    /* Combine game freq with rate-correction pitch */
    Platform_PlaySound(kAudioChannel0 + chanIdx, sampleData, sampleLen,
                       vol * dist, pan, freq * pitchAdj, bps);
}

/* ======================================================================== */
/* Simple (non-positional) sound                                             */
/* ======================================================================== */

void SimplePlaySound(int id)
{
    if (!gPrefs.sound)
        return;

    int entrySize;
    tSound *sound = (tSound *)GetSortedPackEntry(kPackSnds, id, &entrySize);
    if (!sound)
        return;

    UInt32 soundPri = SoundPriority(sound);
    UInt32 soundFlags = SoundFlags(sound);

    UInt32 chanPriority;
    int chanIdx = PickChannel(&chanPriority);

    if (soundFlags & kSoundPriorityHigher) {
        if (chanPriority >= soundPri)
            return;
    } else {
        if (chanPriority > soundPri)
            return;
    }

    sChannelPriority[chanIdx] = soundPri;

    UInt32 ns = SoundNumSamples(sound);
    int sampleIdx = (ns > 0) ? RanInt(0, ns) : 0;
    int sampleLen, bps;
    float pitchAdj;
    const UInt8 *sampleData = GetSampleData(sound, sampleIdx, entrySize,
                                            &sampleLen, &pitchAdj, &bps);
    if (!sampleData || sampleLen <= 0)
        return;

    Platform_PlaySound(kAudioChannel0 + chanIdx, sampleData, sampleLen,
                       gVolume, 0.0f, pitchAdj, bps);
}
