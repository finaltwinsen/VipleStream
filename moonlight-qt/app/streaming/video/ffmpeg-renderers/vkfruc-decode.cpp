// VipleStream §J.3.e.2.i.8 — VkFruc native VK_KHR_video_decode glue impl.

#include "vkfruc-decode.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

// §J.3.e.2.i.8 — NvVideoParser headers MUST come before vkfruc.h
// (which transitively includes streaming/video/decoder.h's
// `#define MAX_SLICES 4` macro that would clobber NvVideoParser's
// `enum { MAX_SLICES = 8192 }` syntax).
#include "NvVideoParser/VulkanVideoDecoder.h"

#include "vkfruc.h"
#include <SDL.h>
#include <cstring>
#include <vector>

// §J.3.e.2.i.8 Phase 1.3a — GPU-backed bitstream buffer.
//
// VkBuffer (VIDEO_DECODE_SRC_BIT_KHR) + host-visible+coherent VkDeviceMemory,
// persistently mapped.  Parser writes NAL bytes through GetDataPtr (returns the
// mapped host pointer); decode queue reads via GetBuffer + bitstreamBufferOffset
// during vkCmdDecodeVideoKHR (Phase 1.3d).
//
// 19 pure virtuals on VulkanBitstreamBuffer all wired against the mapped pointer
// so the parser sees byte-addressable memory just like the Phase 1.1c CPU version.
// FlushRange/InvalidateRange are no-ops because the memory is HOST_COHERENT.
//
// Allocation is via VkFrucRenderer::createGpuBitstreamBuffer (which knows the
// codec profile + queue family + memory props).  Destructor walks back to the
// renderer's destroyGpuBitstreamBuffer for unmap+free+destroy.  Anonymous
// namespace isolates link visibility.
namespace {
class VkFrucGpuBitstreamBuffer : public VulkanBitstreamBuffer {
public:
    static std::shared_ptr<VkFrucGpuBitstreamBuffer> Create(VkFrucRenderer* parent,
                                                            VkDeviceSize size,
                                                            VkDeviceSize offsetAlign,
                                                            VkDeviceSize sizeAlign) {
        if (!parent) return nullptr;
        VkBuffer       buf  = VK_NULL_HANDLE;
        VkDeviceMemory mem  = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   actual = 0;
        if (!parent->createGpuBitstreamBuffer(size, buf, mem, mapped, actual)) {
            return nullptr;
        }
        // make_shared with private ctor isn't possible; just `new` and wrap.
        return std::shared_ptr<VkFrucGpuBitstreamBuffer>(new VkFrucGpuBitstreamBuffer(
            parent, buf, mem, mapped, actual, offsetAlign, sizeAlign));
    }
    ~VkFrucGpuBitstreamBuffer() override {
        if (m_Parent && (m_Buffer != VK_NULL_HANDLE || m_Memory != VK_NULL_HANDLE)) {
            m_Parent->destroyGpuBitstreamBuffer(m_Buffer, m_Memory);
        }
    }

    VkDeviceSize GetMaxSize() const override { return m_Size; }
    VkDeviceSize GetOffsetAlignment() const override { return m_OffsetAlign; }
    VkDeviceSize GetSizeAlignment()   const override { return m_SizeAlign;   }

    VkDeviceSize Resize(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset) override {
        if (newSize <= m_Size) return m_Size;
        // Allocate fresh buffer + memory, copy old contents, swap, free old.
        VkBuffer       newBuf  = VK_NULL_HANDLE;
        VkDeviceMemory newMem  = VK_NULL_HANDLE;
        void*          newMap  = nullptr;
        VkDeviceSize   newAct  = 0;
        if (!m_Parent->createGpuBitstreamBuffer(newSize, newBuf, newMem, newMap, newAct)) {
            return m_Size;  // resize failed, keep old
        }
        if (copySize && copyOffset + copySize <= m_Size) {
            memcpy(newMap, (uint8_t*)m_MappedPtr + copyOffset, (size_t)copySize);
        }
        m_Parent->destroyGpuBitstreamBuffer(m_Buffer, m_Memory);
        m_Buffer    = newBuf;
        m_Memory    = newMem;
        m_MappedPtr = newMap;
        m_Size      = newAct;
        return m_Size;
    }
    VkDeviceSize Clone(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset,
                       VkSharedBaseObj<VulkanBitstreamBuffer>& vulkanBitstreamBuffer) override {
        auto cl = Create(m_Parent, newSize, m_OffsetAlign, m_SizeAlign);
        if (!cl) {
            vulkanBitstreamBuffer.reset();
            return 0;
        }
        if (copySize && copyOffset + copySize <= m_Size) {
            memcpy(cl->m_MappedPtr, (uint8_t*)m_MappedPtr + copyOffset, (size_t)copySize);
        }
        vulkanBitstreamBuffer = cl;
        return cl->GetMaxSize();
    }

    int64_t MemsetData(uint32_t value, VkDeviceSize offset, VkDeviceSize size) override {
        if (offset + size > m_Size) return -1;
        memset((uint8_t*)m_MappedPtr + offset, (int)value, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataToBuffer(uint8_t* dst, VkDeviceSize dstOff, VkDeviceSize srcOff, VkDeviceSize size) const override {
        if (srcOff + size > m_Size) return -1;
        memcpy(dst + dstOff, (const uint8_t*)m_MappedPtr + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataToBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& dst, VkDeviceSize dstOff,
                              VkDeviceSize srcOff, VkDeviceSize size) const override {
        if (!dst || srcOff + size > m_Size) return -1;
        VkDeviceSize maxSz = 0;
        uint8_t* dstPtr = dst->GetDataPtr(dstOff, maxSz);
        if (!dstPtr || size > maxSz) return -1;
        memcpy(dstPtr, (const uint8_t*)m_MappedPtr + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataFromBuffer(const uint8_t* src, VkDeviceSize srcOff, VkDeviceSize dstOff, VkDeviceSize size) override {
        if (dstOff + size > m_Size) return -1;
        memcpy((uint8_t*)m_MappedPtr + dstOff, src + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataFromBuffer(const VkSharedBaseObj<VulkanBitstreamBuffer>& src, VkDeviceSize srcOff,
                                VkDeviceSize dstOff, VkDeviceSize size) override {
        if (!src || dstOff + size > m_Size) return -1;
        VkDeviceSize maxSz = 0;
        const uint8_t* srcPtr = src->GetReadOnlyDataPtr(srcOff, maxSz);
        if (!srcPtr || size > maxSz) return -1;
        memcpy((uint8_t*)m_MappedPtr + dstOff, srcPtr, (size_t)size);
        return (int64_t)size;
    }
    uint8_t* GetDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) override {
        if (offset >= m_Size) { maxSize = 0; return nullptr; }
        maxSize = m_Size - offset;
        return (uint8_t*)m_MappedPtr + offset;
    }
    const uint8_t* GetReadOnlyDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) const override {
        if (offset >= m_Size) { maxSize = 0; return nullptr; }
        maxSize = m_Size - offset;
        return (const uint8_t*)m_MappedPtr + offset;
    }
    // HOST_COHERENT memory — host writes are immediately GPU-visible.
    void FlushRange(VkDeviceSize, VkDeviceSize) const override {}
    void InvalidateRange(VkDeviceSize, VkDeviceSize) const override {}
    VkBuffer       GetBuffer()       const override { return m_Buffer; }
    VkDeviceMemory GetDeviceMemory() const override { return m_Memory; }

    uint32_t AddStreamMarker(uint32_t off) override { m_Markers.push_back(off); return (uint32_t)m_Markers.size() - 1; }
    uint32_t SetStreamMarker(uint32_t off, uint32_t idx) override {
        if (idx >= m_Markers.size()) m_Markers.resize(idx + 1, 0);
        m_Markers[idx] = off;
        return idx;
    }
    uint32_t GetStreamMarker(uint32_t idx) const override { return idx < m_Markers.size() ? m_Markers[idx] : 0; }
    uint32_t GetStreamMarkersCount() const override { return (uint32_t)m_Markers.size(); }
    const uint32_t* GetStreamMarkersPtr(uint32_t startIdx, uint32_t& maxCount) const override {
        if (startIdx >= m_Markers.size()) { maxCount = 0; return nullptr; }
        maxCount = (uint32_t)m_Markers.size() - startIdx;
        return m_Markers.data() + startIdx;
    }
    uint32_t ResetStreamMarkers() override { m_Markers.clear(); return 0; }

private:
    VkFrucGpuBitstreamBuffer(VkFrucRenderer* parent, VkBuffer buf, VkDeviceMemory mem,
                             void* mapped, VkDeviceSize size,
                             VkDeviceSize offAlign, VkDeviceSize szAlign)
        : m_Parent(parent), m_Buffer(buf), m_Memory(mem), m_MappedPtr(mapped),
          m_Size(size), m_OffsetAlign(offAlign), m_SizeAlign(szAlign) {}

    VkFrucRenderer*       m_Parent      = nullptr;
    VkBuffer              m_Buffer      = VK_NULL_HANDLE;
    VkDeviceMemory        m_Memory      = VK_NULL_HANDLE;
    void*                 m_MappedPtr   = nullptr;
    VkDeviceSize          m_Size        = 0;
    VkDeviceSize          m_OffsetAlign = 0;
    VkDeviceSize          m_SizeAlign   = 0;
    std::vector<uint32_t> m_Markers;
};
} // namespace


// §J.3.e.2.i.8 Phase 1.1c — Pimpl 完整定義 (vkfruc.h 只 forward decl).
struct VkFrucRenderer::NvParserPimpl {
    std::shared_ptr<VkFrucDecodeClient>          client;
    VkSharedBaseObj<VulkanVideoDecodeParser>     parser;
};


// =============================================================================
// VkFrucDecodeClient — VkParserVideoDecodeClient impl
// =============================================================================

int32_t VkFrucDecodeClient::BeginSequence(const VkParserSequenceInfo* pnvsi)
{
    m_BeginSeqCount.fetch_add(1);
    if (!pnvsi) return 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 parser BeginSequence #%llu — "
                "codec=%d coded=%dx%d display=%dx%d bit_depth_luma=%u",
                (unsigned long long)m_BeginSeqCount.load(),
                (int)pnvsi->eCodec,
                pnvsi->nCodedWidth, pnvsi->nCodedHeight,
                pnvsi->nDisplayWidth, pnvsi->nDisplayHeight,
                (unsigned)pnvsi->uBitDepthLumaMinus8 + 8);

    // §J.3.e.2.i.8 Phase 1.3b — provision DPB image pool at the actual stream
    // resolution.  Idempotent across IDR resends; teardown clears m_DpbReady.
    // 若 stream 維度變了 (rare), Phase 1.3 之後可能需要 recreate VkVideoSession
    // — 目前先信任 first BeginSequence 的尺寸.
    if (m_Parent && pnvsi->nCodedWidth > 0 && pnvsi->nCodedHeight > 0) {
        m_Parent->ensureDpbImagePool(pnvsi->nCodedWidth, pnvsi->nCodedHeight);
    }
    return 16;  // max ref frames (H.265 spec)
}

bool VkFrucDecodeClient::AllocPictureBuffer(VkPicIf** ppPicBuf)
{
    if (!ppPicBuf) return false;
    // §J.3.e.2.i.8 Phase 1.1c — 從 pool 找空閒 slot (refCount == 0).
    // Phase 1.3 換成 VkImage-backed DPB pool.
    for (int i = 0; i < kPicPoolSize; i++) {
        if (m_PicPool[i].IsAvailable()) {
            m_PicPool[i].m_picIdx = i;
            m_PicPool[i].AddRef();  // parser → DPB hold
            *ppPicBuf = &m_PicPool[i];
            return true;
        }
    }
    *ppPicBuf = nullptr;
    return false;
}

bool VkFrucDecodeClient::UpdatePictureParameters(
    VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersObject,
    VkSharedBaseObj<VkVideoRefCountBase>& /*client*/)
{
    m_UpdateParamsCount.fetch_add(1);
    if (!pictureParametersObject) return false;
    bool isVps, isSps, isPps;
    int32_t vpsId = pictureParametersObject->GetVpsId(isVps);
    int32_t spsId = pictureParametersObject->GetSpsId(isSps);
    int32_t ppsId = pictureParametersObject->GetPpsId(isPps);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 parser UpdatePictureParameters #%llu — "
                "isVPS=%d(id=%d) isSPS=%d(id=%d) isPPS=%d(id=%d)",
                (unsigned long long)m_UpdateParamsCount.load(),
                (int)isVps, vpsId, (int)isSps, spsId, (int)isPps, ppsId);
    // Phase 1.1c-next: pictureParametersObject->GetStdH265Sps() 之類取出 std
    // struct 後呼叫 vkUpdateVideoSessionParametersKHR 加進 m_VideoSessionParams.
    // 目前先驗證 callback 路徑 work.
    return true;
}

bool VkFrucDecodeClient::DecodePicture(VkParserPictureData* pParserPictureData)
{
    m_DecodePicCount.fetch_add(1);
    if ((m_DecodePicCount.load() % 60) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 parser DecodePicture cumul=%llu",
                    (unsigned long long)m_DecodePicCount.load());
    }

    // §J.3.e.2.i.8 Phase 1.1d — extract VPS/SPS/PPS std structs the parser
    // resolved for this picture and feed them to vkUpdateVideoSessionParametersKHR.
    // outOfBandPictureParameters=false 模式下 parser 不單獨呼叫 UpdatePictureParameters
    // callback；active param sets 都從 hevc.pStdVps/pStdSps/pStdPps 帶過來。
    if (m_Parent && pParserPictureData) {
        const auto& hevc = pParserPictureData->CodecSpecific.hevc;
        m_Parent->onH265PictureParametersFromParser(hevc.pStdVps, hevc.pStdSps, hevc.pStdPps);

        // §J.3.e.2.i.8 Phase 1.3d — record + submit vkCmdDecodeVideoKHR for IDR
        // frames (P/B-frame reference handling lands in 1.3d.2).  Skips silently
        // when prerequisites aren't ready yet (DPB pool, cmd buf, etc).
        m_Parent->submitDecodeFrame(pParserPictureData);
    }

    return true;
}

bool VkFrucDecodeClient::DisplayPicture(VkPicIf* /*pPicBuf*/, int64_t /*llPTS*/)
{
    m_DisplayPicCount.fetch_add(1);
    // Phase 1.4 — 把 decoded picture 接到 graphics pipeline.
    return true;
}

void VkFrucDecodeClient::UnhandledNALU(const uint8_t* /*pbData*/, size_t /*cbData*/)
{
    // 不需要處理 (上游 parser 該 cover 的都 cover 了)
}

VkDeviceSize VkFrucDecodeClient::GetBitstreamBuffer(
    VkDeviceSize size,
    VkDeviceSize minBitstreamBufferOffsetAlignment,
    VkDeviceSize minBitstreamBufferSizeAlignment,
    const uint8_t* pInitializeBufferMemory,
    VkDeviceSize initializeBufferMemorySize,
    VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer)
{
    // §J.3.e.2.i.8 Phase 1.3a — GPU-backed bitstream buffer.
    // Parser writes NAL bytes via the persistent host mapping; decode queue
    // reads via VkBuffer handle in vkCmdDecodeVideoKHR (Phase 1.3d).
    if (!m_Parent) {
        bitstreamBuffer.reset();
        return 0;
    }
    auto buf = VkFrucGpuBitstreamBuffer::Create(
        m_Parent, size, minBitstreamBufferOffsetAlignment, minBitstreamBufferSizeAlignment);
    if (!buf) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a GetBitstreamBuffer "
                     "GPU alloc failed (size=%llu)", (unsigned long long)size);
        bitstreamBuffer.reset();
        return 0;
    }
    if (pInitializeBufferMemory && initializeBufferMemorySize) {
        VkDeviceSize maxSz = 0;
        uint8_t* dst = buf->GetDataPtr(0, maxSz);
        if (dst && initializeBufferMemorySize <= maxSz) {
            memcpy(dst, pInitializeBufferMemory, (size_t)initializeBufferMemorySize);
        }
    }
    bitstreamBuffer = buf;
    return buf->GetMaxSize();
}


// =============================================================================
// VkFrucRenderer Phase 1.1d — H.265 VPS/SPS/PPS upload to video session params.
// =============================================================================

bool VkFrucRenderer::onH265PictureParametersFromParser(
    const StdVideoPictureParametersSet* vps,
    const StdVideoPictureParametersSet* sps,
    const StdVideoPictureParametersSet* pps)
{
    if (m_VideoSessionParams == VK_NULL_HANDLE) return false;

    // Lazy-cache the PFN once the session params object exists.
    if (!m_pfnUpdateVideoSessionParams) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        m_pfnUpdateVideoSessionParams =
            (PFN_vkUpdateVideoSessionParametersKHR)getDevPa(
                m_Device, "vkUpdateVideoSessionParametersKHR");
        if (!m_pfnUpdateVideoSessionParams) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.1d "
                         "vkUpdateVideoSessionParametersKHR PFN missing");
            return false;
        }
    }

    // Determine which sets are NEW (not yet uploaded for that ID, or seen with
    // a strictly higher GetUpdateSequenceCount).  Parser owns the underlying
    // std structs; we copy pointers into the AddInfo, then call vkUpdate before
    // returning to the parser callback (driver copies internally).
    StdVideoH265VideoParameterSet    vpsCopy{};
    StdVideoH265SequenceParameterSet spsCopy{};
    StdVideoH265PictureParameterSet  ppsCopy{};
    uint32_t addVps = 0, addSps = 0, addPps = 0;

    auto isNew = [](std::map<int, uint32_t>& seen, int id, uint32_t seq) {
        auto it = seen.find(id);
        if (it == seen.end())   return true;          // first time
        return seq > it->second;                      // strictly newer
    };

    if (vps) {
        bool isVps = false;
        int  id    = vps->GetVpsId(isVps);
        uint32_t seq = vps->GetUpdateSequenceCount();
        if (isVps && id >= 0) {
            const auto* p = vps->GetStdH265Vps();
            if (p && isNew(m_H265VpsSeqSeen, id, seq)) {
                vpsCopy = *p;
                addVps  = 1;
            }
        }
    }
    if (sps) {
        bool isSps = false;
        int  id    = sps->GetSpsId(isSps);
        uint32_t seq = sps->GetUpdateSequenceCount();
        if (isSps && id >= 0) {
            const auto* p = sps->GetStdH265Sps();
            if (p && isNew(m_H265SpsSeqSeen, id, seq)) {
                spsCopy = *p;
                addSps  = 1;
            }
        }
    }
    if (pps) {
        bool isPps = false;
        int  id    = pps->GetPpsId(isPps);
        uint32_t seq = pps->GetUpdateSequenceCount();
        if (isPps && id >= 0) {
            const auto* p = pps->GetStdH265Pps();
            if (p && isNew(m_H265PpsSeqSeen, id, seq)) {
                ppsCopy = *p;
                addPps  = 1;
            }
        }
    }

    if (!addVps && !addSps && !addPps) {
        // All param sets already uploaded with current seq — no-op.
        return true;
    }

    VkVideoDecodeH265SessionParametersAddInfoKHR h265Add = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR
    };
    h265Add.stdVPSCount = addVps;
    h265Add.pStdVPSs    = addVps ? &vpsCopy : nullptr;
    h265Add.stdSPSCount = addSps;
    h265Add.pStdSPSs    = addSps ? &spsCopy : nullptr;
    h265Add.stdPPSCount = addPps;
    h265Add.pStdPPSs    = addPps ? &ppsCopy : nullptr;

    VkVideoSessionParametersUpdateInfoKHR updateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR
    };
    updateInfo.pNext               = &h265Add;
    // Spec: monotonically increasing, starts at 1 for the first update.
    updateInfo.updateSequenceCount = ++m_H265SessionParamsSeq;

    VkResult vr = m_pfnUpdateVideoSessionParams(
        m_Device, m_VideoSessionParams, &updateInfo);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.1d "
                     "vkUpdateVideoSessionParametersKHR rc=%d "
                     "(addVPS=%u addSPS=%u addPPS=%u seq=%u)",
                     (int)vr, addVps, addSps, addPps, m_H265SessionParamsSeq);
        // Roll back our local seq counter so a retry has a chance to succeed.
        --m_H265SessionParamsSeq;
        return false;
    }

    // Mark uploaded.  Update tracking AFTER the successful vkUpdate.
    if (addVps) {
        bool isVps = false;
        int  id    = vps->GetVpsId(isVps);
        m_H265VpsSeqSeen[id] = vps->GetUpdateSequenceCount();
    }
    if (addSps) {
        bool isSps = false;
        int  id    = sps->GetSpsId(isSps);
        m_H265SpsSeqSeen[id] = sps->GetUpdateSequenceCount();
    }
    if (addPps) {
        bool isPps = false;
        int  id    = pps->GetPpsId(isPps);
        m_H265PpsSeqSeen[id] = pps->GetUpdateSequenceCount();
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.1d "
                "vkUpdateVideoSessionParametersKHR ok — added VPS=%u SPS=%u PPS=%u, "
                "seqCount=%u (cumul VPS-ids=%zu SPS-ids=%zu PPS-ids=%zu)",
                addVps, addSps, addPps, m_H265SessionParamsSeq,
                m_H265VpsSeqSeen.size(), m_H265SpsSeqSeen.size(), m_H265PpsSeqSeen.size());
    return true;
}


// =============================================================================
// VkFrucRenderer Phase 1.3d — per-frame vkCmdDecodeVideoKHR submission
// =============================================================================

bool VkFrucRenderer::loadDecodeRtPfns()
{
    if (m_DecodeRtPfnReady) return true;
    if (m_Device == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto& p = m_DecodeRtPfn;
    p.BeginCommandBuffer       = (PFN_vkBeginCommandBuffer)getDevPa(m_Device, "vkBeginCommandBuffer");
    p.EndCommandBuffer         = (PFN_vkEndCommandBuffer)getDevPa(m_Device, "vkEndCommandBuffer");
    p.ResetCommandBuffer       = (PFN_vkResetCommandBuffer)getDevPa(m_Device, "vkResetCommandBuffer");
    // Sync2: vkCmdPipelineBarrier2 was core in Vulkan 1.3; if absent we fall
    // back to the KHR alias (sync2 extension promoted into core).
    p.CmdPipelineBarrier2      = (PFN_vkCmdPipelineBarrier2)getDevPa(m_Device, "vkCmdPipelineBarrier2");
    if (!p.CmdPipelineBarrier2) p.CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)getDevPa(m_Device, "vkCmdPipelineBarrier2KHR");
    p.CmdBeginVideoCodingKHR   = (PFN_vkCmdBeginVideoCodingKHR)getDevPa(m_Device, "vkCmdBeginVideoCodingKHR");
    p.CmdEndVideoCodingKHR     = (PFN_vkCmdEndVideoCodingKHR)getDevPa(m_Device, "vkCmdEndVideoCodingKHR");
    p.CmdControlVideoCodingKHR = (PFN_vkCmdControlVideoCodingKHR)getDevPa(m_Device, "vkCmdControlVideoCodingKHR");
    p.CmdDecodeVideoKHR        = (PFN_vkCmdDecodeVideoKHR)getDevPa(m_Device, "vkCmdDecodeVideoKHR");
    p.QueueSubmit              = (PFN_vkQueueSubmit)getDevPa(m_Device, "vkQueueSubmit");
    p.WaitForFences            = (PFN_vkWaitForFences)getDevPa(m_Device, "vkWaitForFences");
    p.ResetFences              = (PFN_vkResetFences)getDevPa(m_Device, "vkResetFences");
    if (!p.BeginCommandBuffer || !p.EndCommandBuffer || !p.CmdBeginVideoCodingKHR
        || !p.CmdEndVideoCodingKHR || !p.CmdControlVideoCodingKHR || !p.CmdDecodeVideoKHR
        || !p.CmdPipelineBarrier2 || !p.QueueSubmit || !p.WaitForFences || !p.ResetFences) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d decode rt-PFN missing");
        return false;
    }
    m_DecodeRtPfnReady = true;
    return true;
}

bool VkFrucRenderer::submitDecodeFrame(VkParserPictureData* ppd)
{
    if (!ppd || !m_DpbReady || !m_DecodeCmdReady
        || m_VideoSession == VK_NULL_HANDLE
        || m_VideoSessionParams == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }
    if (!(m_VideoCodec & VIDEO_FORMAT_MASK_H265)) {
        // Phase 1: H.265 only.  Other codecs land in Phase 2+.
        m_DecodeSkipCount++;
        return false;
    }
    if (!loadDecodeRtPfns()) {
        m_DecodeSkipCount++;
        return false;
    }

    const auto& hevc = ppd->CodecSpecific.hevc;

    // pCurrPic is one of our VkFrucDecodeClient::VkFrucDpbPicture instances
    // (handed out by AllocPictureBuffer).
    auto* curPic = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(ppd->pCurrPic);
    if (!curPic || curPic->image == VK_NULL_HANDLE || curPic->view == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }
    int slotIdx = curPic->m_picIdx;

    // §J.3.e.2.i.8 Phase 1.3d.2 — collect active reference DPB slots.
    // hevc.RefPicSetStCurrBefore/After/LtCurr are int8_t[8] containing INDICES
    // INTO hevc.RefPics[16] (NOT directly DPB slot indices).  We dereference
    // RefPics[j] to get the VkFrucDpbPicture* and read its m_picIdx for the
    // actual DPB slot, deduping by slot.
    constexpr int kMaxRefs = 16;
    StdVideoDecodeH265ReferenceInfo refStdInfos[kMaxRefs] = {};
    VkVideoDecodeH265DpbSlotInfoKHR refH265Slots[kMaxRefs] = {};
    VkVideoPictureResourceInfoKHR   refResources[kMaxRefs] = {};
    VkVideoReferenceSlotInfoKHR     refSlots[kMaxRefs]     = {};
    int  activeRefCount = 0;
    bool seenSlot[VkFrucDecodeClient::kPicPoolSize] = {};

    auto collectRef = [&](int j) {
        if (j < 0 || j >= 16) return;
        auto* p = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(hevc.RefPics[j]);
        if (!p || p->image == VK_NULL_HANDLE || p->view == VK_NULL_HANDLE) return;
        int slot = p->m_picIdx;
        if (slot < 0 || slot >= VkFrucDecodeClient::kPicPoolSize) return;
        if (slot == slotIdx) return;        // never include current pic among refs
        if (!m_DpbSlotActive[slot]) return;  // skip slots not yet activated by a prior decode
        if (seenSlot[slot]) return;
        seenSlot[slot] = true;

        auto& sri = refStdInfos[activeRefCount];
        sri.PicOrderCntVal = hevc.PicOrderCntVal[j];
        sri.flags.used_for_long_term_reference = hevc.IsLongTerm[j] ? 1 : 0;
        sri.flags.unused_for_reference         = 0;

        auto& slotH265 = refH265Slots[activeRefCount];
        slotH265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
        slotH265.pStdReferenceInfo = &sri;

        auto& res = refResources[activeRefCount];
        res.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        res.codedOffset      = { 0, 0 };
        res.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
        res.baseArrayLayer   = 0;
        res.imageViewBinding = p->view;

        auto& s = refSlots[activeRefCount];
        s.sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        s.pNext            = &slotH265;
        s.slotIndex        = slot;
        s.pPictureResource = &res;

        activeRefCount++;
    };
    for (int i = 0; i < hevc.NumPocStCurrBefore && i < 8; i++) collectRef(hevc.RefPicSetStCurrBefore[i]);
    for (int i = 0; i < hevc.NumPocStCurrAfter  && i < 8; i++) collectRef(hevc.RefPicSetStCurrAfter[i]);
    for (int i = 0; i < hevc.NumPocLtCurr       && i < 8; i++) collectRef(hevc.RefPicSetLtCurr[i]);

    auto& bsBuf = ppd->bitstreamData;
    if (!bsBuf || bsBuf->GetBuffer() == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }

    // Slice segment offsets — parser stores them as stream markers in the bitstream
    // buffer (one per slice).  numSlices was filled by VulkanVideoDecoder::end_of_picture.
    uint32_t markerCount = 0;
    const uint32_t* sliceOffsets = bsBuf->GetStreamMarkersPtr(ppd->firstSliceIndex, markerCount);
    if (!sliceOffsets || markerCount == 0) {
        m_DecodeSkipCount++;
        return false;
    }
    if (markerCount > ppd->numSlices) markerCount = ppd->numSlices;

    auto& rt = m_DecodeRtPfn;

    // Wait for previous decode submission to complete (single cmd buffer pattern).
    rt.WaitForFences(m_Device, 1, &m_DecodeFence, VK_TRUE, UINT64_MAX);
    rt.ResetFences(m_Device, 1, &m_DecodeFence);
    rt.ResetCommandBuffer(m_DecodeCmdBuf, 0);

    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    rt.BeginCommandBuffer(m_DecodeCmdBuf, &cbi);

    // ── Layout transitions + cache flush via sync2 image barriers ──
    // §J.3.e.2.i.8 Phase 1.3d.2.b — all DPB slots back the same VkImage
    // (NV's HEVC profile lacks SEPARATE_REFERENCE_IMAGES capability).  Each
    // barrier targets a single array layer via subresourceRange.baseArrayLayer.
    {
        constexpr int kMaxBarriers = 1 + kMaxRefs;
        VkImageMemoryBarrier2 barriers[kMaxBarriers] = {};
        int barrierCount = 0;

        auto fillBarrier = [&](VkImage img, uint32_t layer, bool isRefRead, bool initFromUndefined) {
            auto& imb = barriers[barrierCount++];
            imb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            imb.srcStageMask        = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
            imb.srcAccessMask       = initFromUndefined ? 0 : VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
            imb.dstStageMask        = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
            imb.dstAccessMask       = isRefRead ? VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR
                                                : VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
            imb.oldLayout           = initFromUndefined ? VK_IMAGE_LAYOUT_UNDEFINED
                                                        : VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
            imb.newLayout           = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
            imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imb.image               = img;
            imb.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };
        };

        // Setup/dst layer barrier
        fillBarrier(curPic->image, (uint32_t)slotIdx, /*isRefRead*/false,
                    /*initFromUndefined*/!curPic->layoutInited);

        // Reference layer barriers (cache flush from prior write → current read)
        for (int i = 0; i < activeRefCount; i++) {
            fillBarrier(curPic->image, (uint32_t)refSlots[i].slotIndex,
                        /*isRefRead*/true, /*initFromUndefined*/false);
        }

        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = (uint32_t)barrierCount;
        dep.pImageMemoryBarriers    = barriers;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &dep);
    }

    // ── Setup-ref slot (current picture's slot) ──
    StdVideoDecodeH265ReferenceInfo stdRefInfo = {};
    stdRefInfo.PicOrderCntVal = hevc.CurrPicOrderCntVal;

    VkVideoDecodeH265DpbSlotInfoKHR setupSlotH265 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR };
    setupSlotH265.pStdReferenceInfo = &stdRefInfo;

    VkVideoPictureResourceInfoKHR setupResource = { VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
    setupResource.codedOffset      = { 0, 0 };
    setupResource.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
    setupResource.baseArrayLayer   = 0;          // imageView already selects the layer
    setupResource.imageViewBinding = curPic->view;

    // Spec: in BeginCoding's pReferenceSlots, an entry with slotIndex=-1 means
    // "this slot is being activated/re-activated in this scope".  Active slots
    // (already set up) use their actual slotIndex.
    // DecodeInfo.pSetupReferenceSlot always uses the actual target slotIndex.
    VkVideoReferenceSlotInfoKHR setupSlotInBegin = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
    setupSlotInBegin.pNext            = &setupSlotH265;
    setupSlotInBegin.slotIndex        = m_DpbSlotActive[slotIdx] ? slotIdx : -1;
    setupSlotInBegin.pPictureResource = &setupResource;

    VkVideoReferenceSlotInfoKHR setupSlotInDecode = setupSlotInBegin;
    setupSlotInDecode.slotIndex = slotIdx;

    // ── Begin video coding ──
    // BeginCoding lists all DPB slots used in this scope: the setup slot
    // (slotIndex=-1 if first activation, else slotIdx) + every active reference.
    VkVideoReferenceSlotInfoKHR allCodingSlots[1 + kMaxRefs];
    allCodingSlots[0] = setupSlotInBegin;
    for (int i = 0; i < activeRefCount; i++) allCodingSlots[1 + i] = refSlots[i];

    VkVideoBeginCodingInfoKHR bci = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    bci.videoSession           = m_VideoSession;
    bci.videoSessionParameters = m_VideoSessionParams;
    bci.referenceSlotCount     = (uint32_t)(1 + activeRefCount);
    bci.pReferenceSlots        = allCodingSlots;
    rt.CmdBeginVideoCodingKHR(m_DecodeCmdBuf, &bci);

    // ── Reset session state on first submit ──
    if (m_DecodeNeedsReset) {
        VkVideoCodingControlInfoKHR cci = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR };
        cci.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        rt.CmdControlVideoCodingKHR(m_DecodeCmdBuf, &cci);
        m_DecodeNeedsReset = false;
        // Session RESET deactivates all DPB slots — sync our tracking.
        for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) m_DpbSlotActive[i] = false;
    }

    // ── Decode ──
    StdVideoDecodeH265PictureInfo stdPicInfo = {};
    stdPicInfo.flags.IrapPicFlag                     = hevc.IrapPicFlag;
    stdPicInfo.flags.IdrPicFlag                      = hevc.IdrPicFlag;
    stdPicInfo.flags.IsReference                     = ppd->ref_pic_flag;
    stdPicInfo.flags.short_term_ref_pic_set_sps_flag = hevc.short_term_ref_pic_set_sps_flag;
    stdPicInfo.sps_video_parameter_set_id            = hevc.vps_video_parameter_set_id;
    stdPicInfo.pps_seq_parameter_set_id              = hevc.seq_parameter_set_id;
    stdPicInfo.pps_pic_parameter_set_id              = hevc.pic_parameter_set_id;
    stdPicInfo.NumDeltaPocsOfRefRpsIdx               = (uint8_t)hevc.NumDeltaPocsOfRefRpsIdx;
    stdPicInfo.PicOrderCntVal                        = hevc.CurrPicOrderCntVal;
    stdPicInfo.NumBitsForSTRefPicSetInSlice          = (uint16_t)hevc.NumBitsForShortTermRPSInSlice;
    // §J.3.e.2.i.8 Phase 1.3d.2 — fill RefPicSet*StCurrBefore/After/LtCurr arrays.
    // These uint8_t[8] entries are DPB slot indices (NOT indices into RefPics[]).
    // Convert via: parser_idx → RefPics[parser_idx] → VkFrucDpbPicture::m_picIdx.
    // Unused entries left as 0xFF defensively (driver should ignore beyond active count).
    for (int i = 0; i < 8; i++) {
        stdPicInfo.RefPicSetStCurrBefore[i] = 0xFF;
        stdPicInfo.RefPicSetStCurrAfter[i]  = 0xFF;
        stdPicInfo.RefPicSetLtCurr[i]       = 0xFF;
    }
    auto fillStdRefSlot = [&](uint8_t* dst, const int8_t* src, int count) {
        for (int i = 0; i < count && i < 8; i++) {
            int j = src[i];
            if (j < 0 || j >= 16) continue;
            auto* p = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(hevc.RefPics[j]);
            if (p && p->m_picIdx >= 0 && p->m_picIdx < VkFrucDecodeClient::kPicPoolSize) {
                dst[i] = (uint8_t)p->m_picIdx;
            }
        }
    };
    fillStdRefSlot(stdPicInfo.RefPicSetStCurrBefore, hevc.RefPicSetStCurrBefore, hevc.NumPocStCurrBefore);
    fillStdRefSlot(stdPicInfo.RefPicSetStCurrAfter,  hevc.RefPicSetStCurrAfter,  hevc.NumPocStCurrAfter);
    fillStdRefSlot(stdPicInfo.RefPicSetLtCurr,       hevc.RefPicSetLtCurr,       hevc.NumPocLtCurr);

    VkVideoDecodeH265PictureInfoKHR h265Pic = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR };
    h265Pic.pStdPictureInfo      = &stdPicInfo;
    h265Pic.sliceSegmentCount    = markerCount;
    h265Pic.pSliceSegmentOffsets = sliceOffsets;

    // Align srcBufferRange up — driver may require minBitstreamBufferSizeAlignment;
    // 256 covers the typical Vulkan requirement (actual cap is queried later if needed).
    VkDeviceSize alignedRange = (ppd->bitstreamDataLen + 255u) & ~VkDeviceSize(255);

    VkVideoDecodeInfoKHR di = { VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR };
    di.pNext                = &h265Pic;
    di.srcBuffer            = bsBuf->GetBuffer();
    di.srcBufferOffset      = ppd->bitstreamDataOffset;
    di.srcBufferRange       = alignedRange;
    di.dstPictureResource   = setupResource;
    di.pSetupReferenceSlot  = &setupSlotInDecode;     // slotIndex = real target (>=0)
    di.referenceSlotCount   = (uint32_t)activeRefCount;
    di.pReferenceSlots      = activeRefCount > 0 ? refSlots : nullptr;
    rt.CmdDecodeVideoKHR(m_DecodeCmdBuf, &di);

    // ── End video coding ──
    VkVideoEndCodingInfoKHR eci = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    rt.CmdEndVideoCodingKHR(m_DecodeCmdBuf, &eci);

    rt.EndCommandBuffer(m_DecodeCmdBuf);

    // §J.3.e.2.i.8 Phase 1.3d.2.b — drop signal semaphore: nothing waits on it
    // in Phase 1.3 (graphics-queue handoff lives in Phase 1.4).  Repeatedly
    // signaling a binary semaphore without a wait between signals is a spec
    // violation (VUID-vkQueueSubmit-pCommandBuffers-00065).  Phase 1.4 will
    // re-introduce signal+wait when the decoded image flows to the renderer.
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_DecodeCmdBuf;
    si.signalSemaphoreCount = 0;
    si.pSignalSemaphores    = nullptr;
    VkResult vr = rt.QueueSubmit(m_DecodeQueue, 1, &si, m_DecodeFence);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d vkQueueSubmit rc=%d", (int)vr);
        m_DecodeSkipCount++;
        return false;
    }

    curPic->layoutInited = true;
    m_DpbSlotActive[slotIdx] = true;   // slot is now usable as a reference
    m_DecodeSubmitCount++;
    if (m_DecodeSubmitCount == 1 || (m_DecodeSubmitCount % 60) == 0) {
        const char* picType = hevc.IdrPicFlag ? "IDR" :
                              hevc.IrapPicFlag ? "IRAP" :
                              activeRefCount == 0 ? "I" : "P/B";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d decode submitted #%llu "
                    "(%s slot=%d refs=%d slices=%u srcRange=%llu skipped=%llu)",
                    (unsigned long long)m_DecodeSubmitCount,
                    picType, slotIdx, activeRefCount, markerCount,
                    (unsigned long long)alignedRange,
                    (unsigned long long)m_DecodeSkipCount);
    }
    return true;
}


// =============================================================================
// VkFrucRenderer Phase 1.3b — DPB image pool (17 NV12 VkImages with profile chain)
// =============================================================================

bool VkFrucRenderer::ensureDpbImagePool(int width, int height)
{
    if (m_DpbReady) return true;
    if (!m_VideoProfileReady || m_Device == VK_NULL_HANDLE) return false;
    if (!m_NvParser || !m_NvParser->client) return false;

    // HEVC CTU sizes are 16/32/64 — round up to 64 to fit any CTB layout.
    auto alignUp = [](int x, int a) { return (x + a - 1) & ~(a - 1); };
    int alignedW = alignUp(width, 64);
    int alignedH = alignUp(height, 64);

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateImage      = (PFN_vkCreateImage)getDevPa(m_Device, "vkCreateImage");
    auto pfnDestroyImage     = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnGetImageMemReq   = (PFN_vkGetImageMemoryRequirements)getDevPa(m_Device, "vkGetImageMemoryRequirements");
    auto pfnAllocMem         = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnFreeMem          = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnBindImageMem     = (PFN_vkBindImageMemory)getDevPa(m_Device, "vkBindImageMemory");
    auto pfnCreateImageView  = (PFN_vkCreateImageView)getDevPa(m_Device, "vkCreateImageView");
    auto pfnDestroyImageView = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnGetMemProps      = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateImage || !pfnGetImageMemReq || !pfnAllocMem || !pfnBindImageMem
        || !pfnCreateImageView || !pfnGetMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b DPB image PFN missing");
        return false;
    }

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetMemProps(m_PhysicalDevice, &memProps);
    auto findDeviceLocal = [&](uint32_t typeBits) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                return (int)i;
            }
        }
        return -1;
    };

    VkVideoProfileListInfoKHR profileList = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
    profileList.profileCount = 1;
    profileList.pProfiles    = &m_VideoProfile;

    auto& pool = m_NvParser->client->m_PicPool;

    // §J.3.e.2.i.8 Phase 1.3d.2.b — single VkImage with arrayLayers=kPicPoolSize.
    // NV HEVC profile lacks VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR,
    // so all reference slots must view into the same backing image.  Per-slot
    // VkImageView selects via baseArrayLayer.  Drop SAMPLED (Phase 1.4 will
    // create a separate sampled view with VkSamplerYcbcrConversion attached).
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext         = &profileList;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ici.extent        = { (uint32_t)alignedW, (uint32_t)alignedH, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = (uint32_t)VkFrucDecodeClient::kPicPoolSize;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR
                      | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult vr = pfnCreateImage(m_Device, &ici, nullptr, &m_DpbSharedImage);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b shared DPB vkCreateImage rc=%d "
                     "%dx%d arrayLayers=%d", (int)vr, alignedW, alignedH,
                     VkFrucDecodeClient::kPicPoolSize);
        destroyDpbImagePool();
        return false;
    }

    VkMemoryRequirements memReq = {};
    pfnGetImageMemReq(m_Device, m_DpbSharedImage, &memReq);
    int mti = findDeviceLocal(memReq.memoryTypeBits);
    if (mti < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b no DEVICE_LOCAL memory type "
                     "(typeBits=0x%x)", memReq.memoryTypeBits);
        destroyDpbImagePool();
        return false;
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = (uint32_t)mti;
    vr = pfnAllocMem(m_Device, &mai, nullptr, &m_DpbSharedMem);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b shared DPB vkAllocateMemory rc=%d size=%llu",
                     (int)vr, (unsigned long long)memReq.size);
        destroyDpbImagePool();
        return false;
    }
    vr = pfnBindImageMem(m_Device, m_DpbSharedImage, m_DpbSharedMem, 0);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b vkBindImageMemory rc=%d", (int)vr);
        destroyDpbImagePool();
        return false;
    }

    // Per-slot views: each selects a single array layer of the shared image.
    for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) {
        pool[i].image  = m_DpbSharedImage;          // shared backing image
        pool[i].memory = VK_NULL_HANDLE;            // memory owned by renderer

        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image                           = m_DpbSharedImage;
        vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                          = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.components                      = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = (uint32_t)i;
        vci.subresourceRange.layerCount     = 1;
        vr = pfnCreateImageView(m_Device, &vci, nullptr, &pool[i].view);
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b vkCreateImageView[%d] rc=%d",
                         i, (int)vr);
            destroyDpbImagePool();
            return false;
        }
    }

    // All slots inactive until they've been pSetupReferenceSlot of a decode submit.
    for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) m_DpbSlotActive[i] = false;

    m_DpbReady    = true;
    m_DpbAlignedW = alignedW;
    m_DpbAlignedH = alignedH;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3b DPB image pool ready — "
                "1 shared VkImage @ %dx%d NV12 × %d array layers (req %dx%d, "
                "DPB|DST usage; sampling deferred to Phase 1.4)",
                alignedW, alignedH, VkFrucDecodeClient::kPicPoolSize, width, height);
    return true;
}

void VkFrucRenderer::destroyDpbImagePool()
{
    if (m_Device == VK_NULL_HANDLE) return;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyImageView = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnDestroyImage     = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnFreeMem          = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");

    // Per-slot views (one per array layer) — only walk if parser/client still alive.
    if (m_NvParser && m_NvParser->client) {
        auto& pool = m_NvParser->client->m_PicPool;
        for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) {
            if (pool[i].view != VK_NULL_HANDLE && pfnDestroyImageView)
                pfnDestroyImageView(m_Device, pool[i].view, nullptr);
            pool[i].view   = VK_NULL_HANDLE;
            pool[i].image  = VK_NULL_HANDLE;  // backing was shared, freed below
            pool[i].memory = VK_NULL_HANDLE;
            pool[i].layoutInited = false;
            pool[i].Reset();
        }
    }
    if (m_DpbSharedImage != VK_NULL_HANDLE && pfnDestroyImage)
        pfnDestroyImage(m_Device, m_DpbSharedImage, nullptr);
    if (m_DpbSharedMem != VK_NULL_HANDLE && pfnFreeMem)
        pfnFreeMem(m_Device, m_DpbSharedMem, nullptr);
    m_DpbSharedImage = VK_NULL_HANDLE;
    m_DpbSharedMem   = VK_NULL_HANDLE;
    m_DpbReady       = false;
    m_DpbAlignedW    = 0;
    m_DpbAlignedH    = 0;
    for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) m_DpbSlotActive[i] = false;
}


// =============================================================================
// VkFrucRenderer parser instance management (defined here to avoid pulling
// nvvideoparser headers into vkfruc.cpp's already-massive #include set)
// =============================================================================

bool VkFrucRenderer::createNvVideoParser()
{
    // §J.3.e.2.i.8 Phase 1 — H.265 ONLY.  nvvideoparser library compiled with
    // VIPLESTREAM_NVPARSER_H265_ONLY define strips out H.264/AV1/VP9 dispatch
    // cases (see nvvideoparser.pro:41).  Trying CreateVulkanVideoDecodeParser
    // with non-H265 codec falls through to the switch's default branch and
    // dereferences a null shared_ptr → crash.  We hard-skip parser instantiation
    // for non-H265 streams and let session/cmd resources stay live (graphics
    // path still runs SW NV12 upload, just no native decode acceleration).
    // Phase 2 ports H.264; Phase 3 ports AV1.
    if (!(m_VideoCodec & VIDEO_FORMAT_MASK_H265)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createNvVideoParser SKIPPED — "
                    "current stream codec mask=0x%x is not H.265 (Phase 1 supports H.265 only)",
                    m_VideoCodec);
        return false;  // caller logs warning + falls back to legacy NAL-counter path
    }

    auto pimpl = new NvParserPimpl();
    pimpl->client = std::make_shared<VkFrucDecodeClient>(this);

    VkExtensionProperties stdHeaderVer = {};
    VkVideoCodecOperationFlagBitsKHR codecOp;
    if (m_VideoCodec & VIDEO_FORMAT_MASK_H265) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else {
        // Unreachable due to early bail above — keep for future codec phases.
        delete pimpl;
        return false;
    }

    VkParserInitDecodeParameters initParams = {};
    initParams.interfaceVersion          = NV_VULKAN_VIDEO_PARSER_API_VERSION;
    initParams.pClient                   = pimpl->client.get();
    initParams.defaultMinBufferSize      = 1024 * 1024;
    initParams.bufferOffsetAlignment     = 64;
    initParams.bufferSizeAlignment       = 64;
    initParams.referenceClockRate        = 10000000;  // 10 MHz default
    initParams.errorThreshold            = 50;
    initParams.outOfBandPictureParameters = false;

    VkResult vr = CreateVulkanVideoDecodeParser(
        codecOp, &stdHeaderVer, /*logFunc*/ nullptr, /*logLevel*/ 0,
        &initParams, pimpl->parser);
    if (vr != VK_SUCCESS || !pimpl->parser) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 CreateVulkanVideoDecodeParser rc=%d",
                     (int)vr);
        delete pimpl;
        return false;
    }

    m_NvParser = pimpl;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 NvVideoParser instantiated (%s, std=%s v%u)",
                (codecOp == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) ? "H.265" :
                (codecOp == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) ? "H.264" : "AV1",
                stdHeaderVer.extensionName, stdHeaderVer.specVersion);
    return true;
}

void VkFrucRenderer::destroyNvVideoParser()
{
    if (m_NvParser) {
        // §J.3.e.2.i.8 Phase 1.3b — DPB image pool lives inside the client's
        // m_PicPool[].  Drop those VkImages BEFORE releasing the client
        // (otherwise destroyDpbImagePool can't iterate the pool).
        destroyDpbImagePool();
        // VulkanVideoDecodeParser uses intrusive refcount; reset() drops our
        // reference + destructor calls Deinitialize() automatically.
        m_NvParser->parser.reset();
        m_NvParser->client.reset();
        delete m_NvParser;
        m_NvParser = nullptr;
    }
}

void VkFrucRenderer::feedNalToNvParser(const uint8_t* data, size_t len)
{
    if (!m_NvParser || !m_NvParser->parser || !data || !len) return;
    VkParserBitstreamPacket pck = {};
    pck.pByteStream     = data;
    pck.nDataLength     = len;
    pck.llPTS           = 0;
    pck.bEOS            = 0;
    pck.bPTSValid       = 0;
    pck.bDiscontinuity  = 0;
    pck.bPartialParsing = 0;
    pck.bEOP            = 0;
    pck.pbSideData      = nullptr;
    pck.nSideDataLength = 0;
    size_t parsed = 0;
    bool ok = m_NvParser->parser->ParseByteStream(&pck, &parsed);
    if (!ok) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.8 ParseByteStream returned false (parsed=%zu of %zu)",
                        parsed, len);
        }
    }
}

#endif // HAVE_LIBPLACEBO_VULKAN
