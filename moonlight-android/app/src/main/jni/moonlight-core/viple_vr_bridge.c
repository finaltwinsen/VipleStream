// §QUEST-VR
// 讀寫鎖：呼叫端握讀鎖才讀 sink＋ctx 並呼叫，鎖握到呼叫返回。
// 取消握寫鎖。寫鎖會等所有讀鎖放開，所以 Set 返回之後不會再碰到已釋放的 ctx。
// message 與 frame 各一把鎖，兩邊的回呼不會互相等。

#include "viple_vr_bridge.h"

#include <pthread.h>

typedef struct {
    VipleVrMessageSink sink;
    void* ctx;
} VipleVrMessageSlot;

typedef struct {
    VipleVrFrameSink sink;
    void* ctx;
} VipleVrFrameSlot;

static pthread_rwlock_t g_messageLock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t g_frameLock = PTHREAD_RWLOCK_INITIALIZER;
static VipleVrMessageSlot g_message;
static VipleVrFrameSlot g_frame;

VIPLE_VR_BRIDGE_API void VipleVrBridgeSetMessageSink(VipleVrMessageSink sink, void* ctx) {
    pthread_rwlock_wrlock(&g_messageLock);
    g_message.sink = sink;
    // sink 是 NULL 時不留 ctx，避免呼叫端已經把 ctx 釋放掉我們還指著。
    g_message.ctx = sink != NULL ? ctx : NULL;
    pthread_rwlock_unlock(&g_messageLock);
}

VIPLE_VR_BRIDGE_API void VipleVrBridgeSetFrameSink(VipleVrFrameSink sink, void* ctx) {
    pthread_rwlock_wrlock(&g_frameLock);
    g_frame.sink = sink;
    g_frame.ctx = sink != NULL ? ctx : NULL;
    pthread_rwlock_unlock(&g_frameLock);
}

void VipleVrBridgeOnMessage(const uint8_t* tlv, int length) {
    pthread_rwlock_rdlock(&g_messageLock);
    if (g_message.sink != NULL) {
        g_message.sink(g_message.ctx, tlv, length);
    }
    pthread_rwlock_unlock(&g_messageLock);
}

void VipleVrBridgeOnFrame(const DECODE_UNIT* decodeUnit) {
    if (decodeUnit == NULL) {
        return;
    }
    pthread_rwlock_rdlock(&g_frameLock);
    if (g_frame.sink != NULL) {
        g_frame.sink(g_frame.ctx, decodeUnit->frameNumber, &decodeUnit->vrMeta,
                     decodeUnit->receiveTimeUs, decodeUnit->enqueueTimeUs,
                     decodeUnit->rtpTimestamp, decodeUnit->frameType);
    }
    pthread_rwlock_unlock(&g_frameLock);
}
