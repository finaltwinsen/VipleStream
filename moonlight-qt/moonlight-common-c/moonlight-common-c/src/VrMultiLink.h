// VipleStream 2.0 §VR-MULTILINK：VR session 的多連線（client 端）。
//
// client 的每張網卡各開一條連線（專用的影像／音訊 UDP socket），server 在每條連線各送一份影像與音訊，
// 這裡把所有 socket 收到的封包餵進同一個佇列（重複的由既有的 RTP 序號檢查丟掉）；追蹤（0x5506）加密後
// 在每條連線各送一份。線上格式見 VipleVr.h 的「§VR-MULTILINK」段落。
//
// 只在 StreamConfig.vrFlags 帶 VIPLE_VR_SF_MULTILINK（server 在 /launch 回了 multilink=1）時啟用；
// 沒啟用時所有函式都是 no-op，影像／音訊／追蹤走原本的單一路徑（行為完全不變）。
// 網卡列舉依賴 PlatformNetIf，所以只在 VIPLE_MPQUIC 建置有實作。
#pragma once

#include "Limelight-internal.h"

#ifdef __cplusplus
extern "C" {
#endif

// 連線準備：找出連線、建立 socket。在 control stream 連上之後、影像串流啟動之前呼叫。回傳連線數（0＝不啟用）。
int vrmlPrepare(void);
// 送 LINK_HELLO、啟動 ping 執行緒（vrmlPrepare 回傳 > 0 才有作用）。
void vrmlStart(void);
// 停掉 ping 執行緒並關閉 socket。必須在影像與音訊接收執行緒都結束之後呼叫。
void vrmlStop(void);
// 連線數（0＝沒啟用）
int vrmlLinkCount(void);

// ControlStream 收到 0x5508 的 TLV 時呼叫：只處理 LINK_READY，其餘略過。
void vrmlOnS2C(const unsigned char* tlv, int len);

// 影像接收執行緒：同時等原本的 socket 與所有連線的影像 socket。
// 回傳值與 recvUdpSocket 相同（> 0 資料長度、0 逾時、< 0 錯誤）；*linkIdx＝-1 表示原本的 socket，否則是連線索引。
// timeoutMs > 0＝這一次最多等這麼久（佇列有封包在等期限時用）；≤ 0＝預設的 UDP_RECV_POLL_TIMEOUT_MS。
int vrmlRecvVideo(SOCKET legacy, char* buffer, int size, int* linkIdx, int timeoutMs);
// 這個封包被佇列採用（先到的那一份）
void vrmlNoteVideoUsed(int linkIdx);
// 最近（約 200 ms）實際在送影像的連線數：探測中的連線每秒只送一幀，不算。少於 2 時跨幀等待沒有東西可等。
int vrmlVideoCarrying(void);

// §VR-LINK-GRACE：有沒有任何一條連線最近還收得到 PONG（ENet 斷線時用來決定要不要撐住 session）
int vrmlAnyAlive(void);
// 同一份統計的另外兩個面向（只在影像接收執行緒呼叫）：哪幾條在送（bit i＝連線索引 i），以及每條連線平常比最先
// 送到的那條晚多久（us；每一幀比各條連線第一個封包的到達時刻）。§VR-LINK-REPAIR 用來決定回報缺包的時機。
int vrmlVideoCarryMask(void);
uint32_t vrmlVideoLinkLagUs(int linkIdx);
// 音訊接收執行緒：同上，但不分連線
int vrmlRecvAudio(SOCKET legacy, char* buffer, int size);

// 追蹤：加密後在每條已確認的連線各送一份。回傳送出的份數；0＝沒有可用連線，呼叫端改走 ENet。
int vrmlSendTracking(const void* sample, int length);

// §VR-LINK-CTRL：server 同意的話（LINK_READY 尾端的 hubCaps），把一則 0x5507 TLV 加密後在每條已確認的連線各送一份。
// 回傳送出的份數；0＝沒協商這個功能或沒有可用連線，呼叫端改走 ENet。只給時間敏感、掉了也無妨的訊息（LOSS、LATCH、NACK）：
// session 位址那條鏈路變弱時，這些訊息走 ENet 會掉或晚到。非阻塞，不碰 enetMutex，任何執行緒都可以呼叫。
int vrmlSendCtrl(const unsigned char* tlv, int length);
// 已確認連線裡最小的往返時間（ms，無條件進位）；沒協商 §VR-LINK-CTRL 或沒有已確認的連線時回 -1
int vrmlCtrlRttMs(void);
// server 同意啟用的連線層功能（VIPLE_VR_LINK_F_*；LINK_READY 之前是 0）
int vrmlFeatures(void);
// §VR-LINK-REPAIR：server 回了 REPAIR_GONE（某個 block 補不了）時取出一次。只在影像接收執行緒呼叫。
bool vrmlTakeGone(uint32_t* frame, uint8_t* block);

#ifdef __cplusplus
}
#endif
