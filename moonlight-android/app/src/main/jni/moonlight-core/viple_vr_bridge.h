// §QUEST-VR
// viple-openxr（另一個 .so）向 moonlight-core 登記回呼，拿走 0x5508 與每一幀的 vrMeta。
// 不把這兩條路送進 Java。tracking／LiSendVr* 由那個模組直接呼叫 common-c，這裡不包裝。
//
// 執行緒：回呼發生在 common-c 的執行緒上，不可阻塞，也不可再呼叫 VipleVrBridgeSet*。
// 原因見 viple_vr_bridge.c：取消要等進行中的呼叫結束，同一條執行緒再拿寫鎖會死結。
//
// message 的形狀跟 Limelight.h 的 ConnListenerVrMessage 一樣（整包 TLV，不是拆開的 type＋payload）。
// 一則 0x5508 可以串多筆 {u8 subtype, u8 len, payload}，在這裡拆開會丟掉後續筆。
// tlv 與 meta 都只在回呼返回前有效。

#ifndef VIPLE_VR_BRIDGE_H
#define VIPLE_VR_BRIDGE_H

#include <Limelight.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define VIPLE_VR_BRIDGE_API __attribute__((visibility("default")))
#else
#define VIPLE_VR_BRIDGE_API
#endif

/* VR 場次 Java 送進 MediaCodec 的 PTS＝frameNumber×這個步距（微秒）。輸出影像的時間戳除回來就是幀號，
   用它查 frame sink 收到的 vrMeta。必須與 MediaCodecDecoderRenderer.VR_PTS_STEP_US 相同。 */
#define VIPLE_VR_BRIDGE_PTS_STEP_US 8000

typedef void (*VipleVrMessageSink)(void* ctx, const uint8_t* tlv, int length);

typedef void (*VipleVrFrameSink)(void* ctx, int frameNumber, const VIPLE_VR_FRAME_META* meta,
                                 uint64_t receiveTimeUs, uint64_t enqueueTimeUs,
                                 uint32_t rtpTimestamp, int frameType);

/* 傳 NULL 取消。sink 與 ctx 一起換；取消返回後不會再拿舊的 ctx 呼叫。 */
VIPLE_VR_BRIDGE_API void VipleVrBridgeSetMessageSink(VipleVrMessageSink sink, void* ctx);
VIPLE_VR_BRIDGE_API void VipleVrBridgeSetFrameSink(VipleVrFrameSink sink, void* ctx);

/* moonlight-core 內部。viple-openxr 不要呼叫。 */
void VipleVrBridgeOnMessage(const uint8_t* tlv, int length);
void VipleVrBridgeOnFrame(const DECODE_UNIT* decodeUnit);

#ifdef __cplusplus
}
#endif

#endif
