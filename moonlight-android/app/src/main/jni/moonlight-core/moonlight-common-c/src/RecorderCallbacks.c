#ifdef _WIN32
// Don't warn for fopen() usage
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "Limelight-internal.h"

static FILE* videoFile;
static FILE* audioFile;

static DECODER_RENDERER_CALLBACKS realDrCallbacks;
static AUDIO_RENDERER_CALLBACKS realArCallbacks;

static int recDrSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags)
{
    const char* path = context;

    if (path != NULL) {
        if (videoFile != NULL) {
            fclose(videoFile);
            videoFile = NULL;
        }
        videoFile = fopen(path, "wb");
        if (videoFile == NULL) {
            return -1;
        }
    }
    else {
        Limelog("Video recording will not be enabled - file path not specified in drContext!\n");
    }

    int ret = realDrCallbacks.setup(videoFormat, width, height, redrawRate, NULL, drFlags);
    if (ret != 0 && videoFile != NULL) {
        fclose(videoFile);
        videoFile = NULL;
    }
    return ret;
}

static void recDrCleanup(void)
{
    if (videoFile != NULL) {
        fclose(videoFile);
        videoFile = NULL;
    }

    if (realDrCallbacks.cleanup != NULL) {
        realDrCallbacks.cleanup();
    }
}

static int recDrSubmitDecodeUnit(PDECODE_UNIT decodeUnit)
{
    if (decodeUnit == NULL) {
        return -1;
    }

    if (videoFile != NULL) {
        PLENTRY entry = decodeUnit->bufferList;
        while (entry != NULL) {
            if (entry->data != NULL && entry->length > 0) {
                fwrite(entry->data, 1, entry->length, videoFile);
            }
            entry = entry->next;
        }
    }

    return realDrCallbacks.submitDecodeUnit(decodeUnit);
}

static int recArInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags)
{
    const char* path = context;

    if (path != NULL) {
        if (audioFile != NULL) {
            fclose(audioFile);
            audioFile = NULL;
        }
        audioFile = fopen(path, "wb");
        if (audioFile == NULL) {
            return -1;
        }
    }
    else {
        Limelog("Audio recording will not be enabled - file path not specified in arContext!\n");
    }

    int ret = realArCallbacks.init(audioConfiguration, opusConfig, NULL, arFlags);
    if (ret != 0 && audioFile != NULL) {
        fclose(audioFile);
        audioFile = NULL;
    }
    return ret;
}

static void recArCleanup(void)
{
    if (audioFile != NULL) {
        fclose(audioFile);
        audioFile = NULL;
    }

    if (realArCallbacks.cleanup != NULL) {
        realArCallbacks.cleanup();
    }
}

static void recArDecodeAndPlaySample(char* sampleData, int sampleLength)
{
    if (audioFile != NULL && sampleData != NULL && sampleLength > 0) {
        fwrite(sampleData, 1, sampleLength, audioFile);
    }

    if (realArCallbacks.decodeAndPlaySample != NULL) {
        realArCallbacks.decodeAndPlaySample(sampleData, sampleLength);
    }
}

void setRecorderCallbacks(PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks)
{
    if (drCallbacks != NULL) {
        realDrCallbacks = *drCallbacks;

        drCallbacks->setup = recDrSetup;
        drCallbacks->cleanup = recDrCleanup;
        drCallbacks->submitDecodeUnit = recDrSubmitDecodeUnit;
    }

    if (arCallbacks != NULL) {
        realArCallbacks = *arCallbacks;

        arCallbacks->init = recArInit;
        arCallbacks->cleanup = recArCleanup;
        arCallbacks->decodeAndPlaySample = recArDecodeAndPlaySample;
    }
}
