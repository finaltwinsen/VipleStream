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


// §J.3.e.2.i.8 Phase 3d.4e/h — AV1 OBU stream scanner.
//
// The upstream nvvideoparser library memcpy()s the entire raw packet (which can
// contain OBU_TEMPORAL_DELIMITER + OBU_SEQUENCE_HEADER + OBU_FRAME / OBU_FRAME_HEADER)
// into the per-picture bitstream buffer at offset 0.  Vulkan AV1 decoder is
// supposed to receive ONLY the frame's bitstream (no TD / SEQ_HDR / METADATA);
// the spec says srcBufferOffset/Range scopes that range and frameHeaderOffset
// is relative to it.  NV's driver appears to NOT tolerate trailing leading TD
// and silently miscoumputes — surfacing as the grey-green flicker we hit.
//
// Walk OBUs from the start and return BOTH the OBU_FRAME / OBU_FRAME_HEADER's
// header position and the size of its OBU header (so the caller can shift
// srcBufferOffset to the OBU header and set frameHeaderOffset to skip past it).
// Returns false if not found.
struct Av1FrameObuLocation {
    uint32_t obuHeaderPos;   // offset of OBU header byte in bitstream
    uint32_t obuHeaderBytes; // bytes of OBU header (1 + ext + leb128 size)
};

static bool scanForAv1FrameObu(const uint8_t* data, size_t size,
                               Av1FrameObuLocation* out)
{
    if (!data || size == 0 || !out) return false;
    size_t pos = 0;
    while (pos < size) {
        uint8_t obuHdr = data[pos];
        uint8_t obuType  = (uint8_t)((obuHdr >> 3) & 0x0F);
        bool    hasExt   = (obuHdr & 0x04) != 0;
        bool    hasSize  = (obuHdr & 0x02) != 0;
        size_t  hdrBytes = 1u + (hasExt ? 1u : 0u);

        // Read leb128 obu_size if present.
        uint64_t obuSize = 0;
        if (hasSize) {
            int shift = 0;
            for (int i = 0; i < 8; i++) {
                if (pos + hdrBytes >= size) return false;
                uint8_t b = data[pos + hdrBytes];
                obuSize |= (uint64_t)(b & 0x7Fu) << shift;
                hdrBytes++;
                if ((b & 0x80u) == 0) break;
                shift += 7;
            }
        } else {
            obuSize = size - (pos + hdrBytes);
        }

        if (obuType == 3 /* FRAME_HEADER */ || obuType == 6 /* FRAME */) {
            out->obuHeaderPos   = (uint32_t)pos;
            out->obuHeaderBytes = (uint32_t)hdrBytes;
            return true;
        }

        if (obuSize > size - (pos + hdrBytes)) return false;
        pos += hdrBytes + (size_t)obuSize;
    }
    return false;
}


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
    //
    // Phase 3a — gate the union access by codec.  CodecSpecific is a union
    // (hevc / av1 / h264 / vp9 share storage); reading hevc fields when the
    // stream is AV1 reinterprets AV1 picture-data bytes as HEVC pointers
    // → garbage pointer → crash inside onH265PictureParametersFromParser.
    if (m_Parent && pParserPictureData) {
        const int codecMask = m_Parent->getVideoCodecMask();
        if (codecMask & VIDEO_FORMAT_MASK_H265) {
            const auto& hevc = pParserPictureData->CodecSpecific.hevc;
            m_Parent->onH265PictureParametersFromParser(hevc.pStdVps, hevc.pStdSps, hevc.pStdPps);
        } else if (codecMask & VIDEO_FORMAT_MASK_H264) {
            // Phase 2 — H.264 has SPS+PPS only (no VPS).  Same incremental
            // add-info upload pattern as H.265.
            const auto& h264 = pParserPictureData->CodecSpecific.h264;
            m_Parent->onH264PictureParametersFromParser(h264.pStdSps, h264.pStdPps);
        } else if (codecMask & VIDEO_FORMAT_MASK_AV1) {
            // Phase 3b.1 — destroy + recreate session params on first real
            // sequence header (replaces the zero-init placeholder built at
            // createVideoSessionParameters time).  Parser re-delivers the
            // same pStdSps every picture; method no-ops on repeat seq counts.
            const auto& av1 = pParserPictureData->CodecSpecific.av1;
            m_Parent->onAv1SequenceHeaderFromParser(av1.pStdSps);
        }

        // §J.3.e.2.i.8 Phase 1.3d — record + submit vkCmdDecodeVideoKHR for IDR
        // frames (P/B-frame reference handling lands in 1.3d.2).  Skips silently
        // when prerequisites aren't ready yet (DPB pool, cmd buf, etc).
        // Phase 3b.2 will lift the H.265-only gate inside submitDecodeFrame
        // and add the AV1 picture-info / DPB / decode-info chain.
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
// VkFrucRenderer Phase 2 — H.264 SPS/PPS upload to video session params.
// =============================================================================
//
// Same incremental add-info upload pattern as onH265PictureParametersFromParser
// minus the VPS branch (H.264 has no VPS — sps_id + pps_id only).  Caches each
// param set's GetUpdateSequenceCount() per id so repeat callbacks no-op.

bool VkFrucRenderer::onH264PictureParametersFromParser(
    const StdVideoPictureParametersSet* sps,
    const StdVideoPictureParametersSet* pps)
{
    if (m_VideoSessionParams == VK_NULL_HANDLE) return false;

    if (!m_pfnUpdateVideoSessionParams) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        m_pfnUpdateVideoSessionParams =
            (PFN_vkUpdateVideoSessionParametersKHR)getDevPa(
                m_Device, "vkUpdateVideoSessionParametersKHR");
        if (!m_pfnUpdateVideoSessionParams) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 "
                         "vkUpdateVideoSessionParametersKHR PFN missing");
            return false;
        }
    }

    StdVideoH264SequenceParameterSet spsCopy{};
    StdVideoH264PictureParameterSet  ppsCopy{};
    uint32_t addSps = 0, addPps = 0;

    auto isNew = [](std::map<int, uint32_t>& seen, int id, uint32_t seq) {
        auto it = seen.find(id);
        if (it == seen.end())   return true;
        return seq > it->second;
    };

    if (sps) {
        bool isSps = false;
        int  id    = sps->GetSpsId(isSps);
        uint32_t seq = sps->GetUpdateSequenceCount();
        if (isSps && id >= 0) {
            const auto* p = sps->GetStdH264Sps();
            if (p && isNew(m_H264SpsSeqSeen, id, seq)) {
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
            const auto* p = pps->GetStdH264Pps();
            if (p && isNew(m_H264PpsSeqSeen, id, seq)) {
                ppsCopy = *p;
                addPps  = 1;
            }
        }
    }

    if (!addSps && !addPps) {
        return true;
    }

    VkVideoDecodeH264SessionParametersAddInfoKHR h264Add = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR
    };
    h264Add.stdSPSCount = addSps;
    h264Add.pStdSPSs    = addSps ? &spsCopy : nullptr;
    h264Add.stdPPSCount = addPps;
    h264Add.pStdPPSs    = addPps ? &ppsCopy : nullptr;

    VkVideoSessionParametersUpdateInfoKHR updateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR
    };
    updateInfo.pNext               = &h264Add;
    updateInfo.updateSequenceCount = ++m_H264SessionParamsSeq;

    VkResult vr = m_pfnUpdateVideoSessionParams(
        m_Device, m_VideoSessionParams, &updateInfo);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 "
                     "vkUpdateVideoSessionParametersKHR rc=%d "
                     "(addSPS=%u addPPS=%u seq=%u)",
                     (int)vr, addSps, addPps, m_H264SessionParamsSeq);
        --m_H264SessionParamsSeq;
        return false;
    }

    if (addSps) {
        bool isSps = false;
        int  id    = sps->GetSpsId(isSps);
        m_H264SpsSeqSeen[id] = sps->GetUpdateSequenceCount();
    }
    if (addPps) {
        bool isPps = false;
        int  id    = pps->GetPpsId(isPps);
        m_H264PpsSeqSeen[id] = pps->GetUpdateSequenceCount();
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 "
                "vkUpdateVideoSessionParametersKHR ok — added SPS=%u PPS=%u, "
                "seqCount=%u (cumul SPS-ids=%zu PPS-ids=%zu)",
                addSps, addPps, m_H264SessionParamsSeq,
                m_H264SpsSeqSeen.size(), m_H264PpsSeqSeen.size());
    return true;
}


// =============================================================================
// VkFrucRenderer Phase 3b.1 — AV1 sequence header upload (destroy+recreate)
// =============================================================================
//
// Vulkan AV1 session parameters are immutable.  Unlike H.265's incremental
// add-info update path, switching the active StdVideoAV1SequenceHeader requires
// destroying and recreating the entire VkVideoSessionParametersKHR object.
// This is called from VkFrucDecodeClient::DecodePicture before submitDecodeFrame,
// so any in-flight decode is already drained (single-cmd-buffer pattern).

bool VkFrucRenderer::onAv1SequenceHeaderFromParser(const StdVideoPictureParametersSet* sps)
{
    if (m_VideoSession == VK_NULL_HANDLE) return false;
    if (!sps) return false;

    // Lazy-resolve PFNs.  We reuse the H.265 path's m_pfnUpdateVideoSessionParams
    // tracking pattern; for AV1 we additionally need create/destroy.
    if (!m_pfnDestroyVideoSessionParams || !m_pfnCreateVideoSessionParams) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        m_pfnDestroyVideoSessionParams = (PFN_vkDestroyVideoSessionParametersKHR)
            getDevPa(m_Device, "vkDestroyVideoSessionParametersKHR");
        m_pfnCreateVideoSessionParams  = (PFN_vkCreateVideoSessionParametersKHR)
            getDevPa(m_Device, "vkCreateVideoSessionParametersKHR");
        if (!m_pfnDestroyVideoSessionParams || !m_pfnCreateVideoSessionParams) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.1 "
                         "create/destroy VideoSessionParameters PFN missing");
            return false;
        }
    }

    const StdVideoAV1SequenceHeader* pStdHeader = sps->GetStdAV1Sps();
    if (!pStdHeader) {
        // Parser handed us a non-AV1 SPS object — defensive bail.
        return false;
    }

    // §J.3.e.2.i.8 Phase 3d.4d — the upstream parser bumps GetUpdateSequenceCount()
    // every time Sunshine resends the OBU_SEQUENCE_HEADER (which it does at
    // ~every keyframe, ≈once per second), even when the actual std header
    // contents are identical.  Acting on the bumped count would re-destroy +
    // recreate VkVideoSessionParametersKHR + force vkCmdControlVideoCodingKHR
    // RESET every second → DPB tracking thrash → alternating valid/garbage
    // frames → grey-green flicker.  Compare the actual StdVideoAV1SequenceHeader
    // bytes against the last-applied snapshot and short-circuit when truly
    // unchanged.  Note: this struct contains pColorConfig / pTimingInfo
    // pointers, which we DO want to follow when comparing — but for repeated
    // re-sends of the same SPS, parser typically points at the same shared
    // data, so memcmp on the parent struct catches pointer equality even
    // when content is also equal (sufficient signal for no-op).
    uint32_t seq = sps->GetUpdateSequenceCount();
    if (m_AV1SeqHdrApplied
        && memcmp(&m_AV1SeqHdrCached, pStdHeader, sizeof(StdVideoAV1SequenceHeader)) == 0) {
        // Same content, regardless of parser-bumped seq count — no-op.
        return true;
    }

    // Copy the header locally so the create-info pointer stays valid through
    // the entire vkCreate call (parser owns the source struct).  Also stash
    // the snapshot for the next-call dedup check.
    StdVideoAV1SequenceHeader hdrCopy = *pStdHeader;
    m_AV1SeqHdrCached = *pStdHeader;

    VkVideoDecodeAV1SessionParametersCreateInfoKHR av1ParamsCi = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    av1ParamsCi.pStdSequenceHeader = &hdrCopy;

    VkVideoSessionParametersCreateInfoKHR vspCi = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    vspCi.pNext                          = &av1ParamsCi;
    vspCi.videoSession                   = m_VideoSession;
    vspCi.videoSessionParametersTemplate = VK_NULL_HANDLE;

    VkVideoSessionParametersKHR newParams = VK_NULL_HANDLE;
    VkResult vr = m_pfnCreateVideoSessionParams(m_Device, &vspCi, nullptr, &newParams);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.1 "
                     "vkCreateVideoSessionParametersKHR(AV1, real seq header) rc=%d",
                     (int)vr);
        return false;
    }

    // Swap in the real-header params and destroy the old (placeholder or
    // previous-seq) one.  Caller is responsible for ensuring no in-flight
    // decode references m_VideoSessionParams — currently guaranteed because
    // submitDecodeFrame drains via m_DecodeFence wait before recording.
    VkVideoSessionParametersKHR oldParams = m_VideoSessionParams;
    m_VideoSessionParams = newParams;
    if (oldParams != VK_NULL_HANDLE) {
        m_pfnDestroyVideoSessionParams(m_Device, oldParams, nullptr);
    }

    m_AV1SeqHdrSeqSeen = seq;
    m_AV1SeqHdrApplied = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.1 — AV1 session params "
                "rebuilt with real sequence header (seq=%u, profile=%u, "
                "max_frame=%ux%u)", seq,
                (unsigned)hdrCopy.seq_profile,
                (unsigned)(hdrCopy.max_frame_width_minus_1 + 1),
                (unsigned)(hdrCopy.max_frame_height_minus_1 + 1));
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
    // §J.3.e.2.i.8 Phase 1.5c — vkWaitSemaphores (Vulkan 1.2 core) for
    // gfx→decode timeline-sem host wait, replacing fence-based sync.
    p.WaitSemaphores           = (PFN_vkWaitSemaphores)getDevPa(m_Device, "vkWaitSemaphores");
    if (!p.WaitSemaphores) p.WaitSemaphores = (PFN_vkWaitSemaphores)getDevPa(m_Device, "vkWaitSemaphoresKHR");
    if (!p.BeginCommandBuffer || !p.EndCommandBuffer || !p.CmdBeginVideoCodingKHR
        || !p.CmdEndVideoCodingKHR || !p.CmdControlVideoCodingKHR || !p.CmdDecodeVideoKHR
        || !p.CmdPipelineBarrier2 || !p.QueueSubmit || !p.WaitForFences || !p.ResetFences
        || !p.WaitSemaphores) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d decode rt-PFN missing");
        return false;
    }
    m_DecodeRtPfnReady = true;
    return true;
}

// =============================================================================
// Phase 3b.2a — submitDecodeFrame top-level dispatcher
// =============================================================================
//
// Universal pre-checks (DPB pool, live session, PFN load) run here; codec-
// specific recording happens in submitDecodeFrameH265 / submitDecodeFrameAv1.
// Each helper assumes its pre-conditions are already met by the dispatcher
// — they should not re-check m_VideoSession / m_VideoSessionParams etc.

bool VkFrucRenderer::submitDecodeFrame(VkParserPictureData* ppd)
{
    // §J.3.e.2.i.8 Phase 1.5c-final — graceful early-out once device-lost
    // detected.  Avoids cascading rc=-4 spam after first detection.
    if (m_DeviceLost.load(std::memory_order_acquire)) {
        m_DecodeSkipCount++;
        return false;
    }
    if (!ppd || !m_DpbReady || !m_DecodeCmdReady
        || m_VideoSession == VK_NULL_HANDLE
        || m_VideoSessionParams == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }
    if (!loadDecodeRtPfns()) {
        m_DecodeSkipCount++;
        return false;
    }

    if (m_VideoCodec & VIDEO_FORMAT_MASK_H265) {
        return submitDecodeFrameH265(ppd);
    }
    if (m_VideoCodec & VIDEO_FORMAT_MASK_H264) {
        return submitDecodeFrameH264(ppd);
    }
    if (m_VideoCodec & VIDEO_FORMAT_MASK_AV1) {
        return submitDecodeFrameAv1(ppd);
    }
    // VP9 / unknown — not supported.
    m_DecodeSkipCount++;
    return false;
}

// =============================================================================
// Phase 3b.2b — AV1 native decode submission body
// =============================================================================
//
// AV1 sibling of submitDecodeFrameH265.  Differences from H.265 path:
//   1. Reference collection — AV1 uses ref_frame_idx[7] indexing pic_idx[8]
//      DPB-position table; no RefPicSetStCurrBefore/After/LtCurr split.
//   2. Picture info — VkVideoDecodeAV1PictureInfoKHR (av1.khr_info) is
//      pre-filled by parser (pStdPictureInfo, referenceNameSlotIndices,
//      tileCount/Offsets/Sizes); we chain it directly into VkVideoDecodeInfoKHR.
//   3. Setup ref slot — av1.setupSlot is pre-filled VkVideoDecodeAV1DpbSlotInfoKHR.
//   4. Reset trigger — av1.needsSessionReset flag from parser (no IRAP/IDR
//      semantic in AV1; sequence header swap causes this in Phase 3b.1).
//   5. Bitstream — AV1 has no slice markers; tile offsets/sizes live inside
//      av1.khr_info.pTileOffsets/pTileSizes (parser-managed, not bsBuf markers).
//
// Decode→SwUpload copy section is codec-agnostic and identical to H.265 path.

bool VkFrucRenderer::submitDecodeFrameAv1(VkParserPictureData* ppd)
{
    // §J.3.e.2.i.8 Phase 3d.3 — AV1 native decode submission is gated behind
    // a separate env var until we finish debugging the v1.3.260 GPU
    // device-lost (decode submit succeeds at CPU, GPU work then faults
    // → grey screen → app crash).  Default OFF means AV1 streams flow
    // through FFmpeg's libdav1d SW decoder + our Vulkan render path
    // (Phase 3a parser instantiation + Phase 3b.1 session-params rebuild
    // still fire, but submitDecodeFrameAv1 short-circuits before we record
    // any vkCmdDecodeVideoKHR for AV1).  Set VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1
    // to opt back in for diagnosis.
    static const bool s_enableAv1Submit =
        qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_AV1_SUBMIT") != 0;
    if (!s_enableAv1Submit) {
        if (m_DecodeSkipCount == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3d.3 — AV1 native "
                        "decode submission DISABLED (set VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1 "
                        "to opt in for diagnosis); falling back to FFmpeg libdav1d");
        }
        m_DecodeSkipCount++;
        return false;
    }

    // §J.3.e.2.i.8 Phase 3d — non-const reference because the parser delivers
    // VkParserAv1PictureData with the *target* sub-structs populated (tileInfo,
    // quantization, segmentation, loopFilter, CDEF, loopRestoration, globalMotion,
    // filmGrain, plus tileInfo's nested MiColStarts/MiRowStarts/...) but does
    // NOT wire the pointer fields inside std_info / tileInfo / setupSlot /
    // dpbSlots[] that the driver dereferences during vkCmdDecodeVideoKHR.
    // Driver hits null → null-deref / GPU device-lost.  We patch them here
    // (safe because the parser owns the struct's lifetime through DecodePicture
    // and no other consumer reads them between our writes and vkCmdDecodeVideoKHR).
    auto& av1 = ppd->CodecSpecific.av1;
    av1.std_info.pTileInfo        = &av1.tileInfo;
    av1.std_info.pQuantization    = &av1.quantization;
    av1.std_info.pSegmentation    = &av1.segmentation;
    av1.std_info.pLoopFilter      = &av1.loopFilter;
    av1.std_info.pCDEF            = &av1.CDEF;
    av1.std_info.pLoopRestoration = &av1.loopRestoration;
    av1.std_info.pGlobalMotion    = &av1.globalMotion;
    av1.std_info.pFilmGrain       = &av1.filmGrain;
    av1.tileInfo.pMiColStarts        = av1.MiColStarts;
    av1.tileInfo.pMiRowStarts        = av1.MiRowStarts;
    av1.tileInfo.pWidthInSbsMinus1   = av1.width_in_sbs_minus_1;
    av1.tileInfo.pHeightInSbsMinus1  = av1.height_in_sbs_minus_1;
    // Phase 3d.4 — setupSlot has sType set but pStdReferenceInfo left null;
    // dpbSlots[8] are entirely zeroed (no sType, no pStdReferenceInfo).
    // Wire them to the parser-prefilled setupSlotInfo / dpbSlotInfos[i] arrays.
    av1.setupSlot.pStdReferenceInfo = &av1.setupSlotInfo;
    for (int i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; i++) {
        av1.dpbSlots[i].sType             = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR;
        av1.dpbSlots[i].pStdReferenceInfo = &av1.dpbSlotInfos[i];
    }

    // §J.3.e.2.i.8 Phase 3b.2b / 3d.4b — parser flags a session reset on
    // sequence-header swap (we already did destroy+recreate in
    // onAv1SequenceHeaderFromParser; the decoder side also needs
    // vkCmdControlVideoCodingKHR(RESET) to discard internal DPB tracking).
    // Drop the >0 count gate so the FIRST AV1 decode after a fresh session
    // params object also gets RESET — without it NV's AV1 decoder appears to
    // reference stale internal state across the params destroy+recreate
    // boundary, manifesting as alternating valid/garbage frames.
    if (av1.needsSessionReset) {
        m_DecodeNeedsReset = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.2b — AV1 needsSessionReset "
                    "(submits so far=%llu) — queueing decoder RESET",
                    (unsigned long long)m_DecodeSubmitCount);
    }

    bool diagThisFrame = (m_DecodeSubmitCount + m_DecodeSkipCount < 5)
                       || av1.needsSessionReset;

    auto* curPic = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(ppd->pCurrPic);
    if (!curPic || curPic->image == VK_NULL_HANDLE || curPic->view == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }
    int slotIdx = curPic->m_picIdx;

    // §J.3.e.2.i.8 Phase 3b.2b — collect active AV1 reference DPB positions.
    // av1.pic_idx[k] is the DPB slot index assigned to AV1 ref_frame buffer
    // position k (or -1 if that buffer is empty).  We iterate all 8 positions
    // and include those that map to a real, currently-active DPB slot.
    // av1.dpbSlots[k] holds the parser-prefilled VkVideoDecodeAV1DpbSlotInfoKHR
    // (with pStdReferenceInfo → av1.dpbSlotInfos[k]) for that position.
    constexpr int kAv1RefSlots = STD_VIDEO_AV1_NUM_REF_FRAMES;  // 8
    VkVideoPictureResourceInfoKHR refResources[kAv1RefSlots] = {};
    VkVideoReferenceSlotInfoKHR   refSlots[kAv1RefSlots]     = {};
    int  activeRefCount = 0;
    bool seenSlot[VkFrucDecodeClient::kPicPoolSize] = {};

    for (int i = 0; i < kAv1RefSlots; i++) {
        int slot = av1.pic_idx[i];
        if (slot < 0 || slot >= VkFrucDecodeClient::kPicPoolSize) continue;
        if (slot == slotIdx) continue;
        if (!m_DpbSlotActive[slot]) continue;
        if (seenSlot[slot]) continue;
        seenSlot[slot] = true;

        auto& res = refResources[activeRefCount];
        res.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        res.codedOffset      = { 0, 0 };
        res.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
        res.baseArrayLayer   = (uint32_t)slot;
        res.imageViewBinding = m_DpbDecodeArrayView;

        auto& s = refSlots[activeRefCount];
        s.sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        s.pNext            = &av1.dpbSlots[i];   // parser-prefilled VkVideoDecodeAV1DpbSlotInfoKHR
        s.slotIndex        = slot;
        s.pPictureResource = &res;

        activeRefCount++;
    }

    auto& bsBuf = ppd->bitstreamData;
    if (!bsBuf || bsBuf->GetBuffer() == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }

    auto& rt = m_DecodeRtPfn;

    // §J.3.e.2.i.8 Phase 1.5c — wait for prior graphics-queue submit (which
    // sampled m_SwUploadImage) to finish before we overwrite it.  Replaces
    // the racey fence-based pattern (m_LastGraphicsFence) which hit
    // VUID-vkResetFences-pFences-01123 when render thread reset the same
    // fence decode thread was waiting on.  Timeline sems support concurrent
    // host waits without reset.
    uint64_t gfxWaitVal = m_LastGraphicsValue.load(std::memory_order_acquire);
    if (gfxWaitVal > 0 && m_GfxTimelineSem != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &m_GfxTimelineSem;
        wi.pValues        = &gfxWaitVal;
        rt.WaitSemaphores(m_Device, &wi, UINT64_MAX);
    }

    rt.WaitForFences(m_Device, 1, &m_DecodeFence, VK_TRUE, UINT64_MAX);
    rt.ResetFences(m_Device, 1, &m_DecodeFence);

    // §J.3.e.2.i.8 Phase 1.7 — see vkfruc.h m_PrevDecodeBsBuf comment.
    // GPU is now drained for the previous decode; safe to release the
    // bitstream buffer we pinned alive across that submit.
    m_PrevDecodeBsBuf.reset();

    rt.ResetCommandBuffer(m_DecodeCmdBuf, 0);

    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    rt.BeginCommandBuffer(m_DecodeCmdBuf, &cbi);

    // ── Layout transitions (single-image + arrayLayers, same as H.265 path) ──
    {
        constexpr int kMaxBarriers = 1 + kAv1RefSlots;
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

        fillBarrier(curPic->image, (uint32_t)slotIdx, /*isRefRead*/false,
                    /*initFromUndefined*/true);
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
    VkVideoPictureResourceInfoKHR setupResource = { VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
    setupResource.codedOffset      = { 0, 0 };
    setupResource.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
    setupResource.baseArrayLayer   = (uint32_t)slotIdx;
    setupResource.imageViewBinding = m_DpbDecodeArrayView;

    VkVideoReferenceSlotInfoKHR setupSlotInBegin = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
    setupSlotInBegin.pNext            = &av1.setupSlot;   // parser-prefilled VkVideoDecodeAV1DpbSlotInfoKHR
    setupSlotInBegin.slotIndex        = -1;
    setupSlotInBegin.pPictureResource = &setupResource;

    VkVideoReferenceSlotInfoKHR setupSlotInDecode = setupSlotInBegin;
    setupSlotInDecode.slotIndex = slotIdx;

    // ── Begin video coding ──
    VkVideoReferenceSlotInfoKHR allCodingSlots[1 + kAv1RefSlots];
    allCodingSlots[0] = setupSlotInBegin;
    for (int i = 0; i < activeRefCount; i++) allCodingSlots[1 + i] = refSlots[i];

    VkVideoBeginCodingInfoKHR bci = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    bci.videoSession           = m_VideoSession;
    bci.videoSessionParameters = m_VideoSessionParams;
    bci.referenceSlotCount     = (uint32_t)(1 + activeRefCount);
    bci.pReferenceSlots        = allCodingSlots;
    rt.CmdBeginVideoCodingKHR(m_DecodeCmdBuf, &bci);

    if (m_DecodeNeedsReset) {
        VkVideoCodingControlInfoKHR cci = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR };
        cci.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        rt.CmdControlVideoCodingKHR(m_DecodeCmdBuf, &cci);
        m_DecodeNeedsReset = false;
        for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) m_DpbSlotActive[i] = false;
    }

    // ── Decode ──
    // §J.3.e.2.i.8 Phase 3b.2b — the parser only populates a small subset of
    // av1.khr_info (tileCount + the tileOffsets/tileSizes arrays' contents);
    // sType, pNext, pStdPictureInfo, pTileOffsets/pTileSizes pointers,
    // referenceNameSlotIndices and frameHeaderOffset all stay zero.  Chaining
    // the raw parser struct hands the driver an effectively-empty picture
    // info → decoder writes all-zero NV12 → green (YUV(0,0,0) → RGB(0,135,0)
    // narrow-range BT.709).  We assemble a fully-populated picture info
    // ourselves; std_info / tileOffsets[] / tileSizes[] data still comes from
    // §J.3.e.2.i.8 Phase 3d.5 — Vulkan AV1 srcBufferOffset semantics:
    // srcBufferOffset is the start of the bitstream (whole packet, can include
    // leading TD / SEQ_HDR), and frameHeaderOffset (in VkVideoDecodeAV1Picture-
    // InfoKHR) tells driver the byte offset to OBU_FRAME / OBU_FRAME_HEADER
    // *header byte itself* (not past it).  Driver internally parses the OBU
    // header (1 byte + optional ext + leb128 size) starting from this offset.
    //
    // Earlier Phase 3d.4h tried to shift srcBufferOffset past TD/SEQ_HDR
    // and use frameHeaderOffset = obuHeaderBytes.  That was wrong on TWO
    // counts: (a) frameHeaderOffset must point AT the OBU header, not past it
    // (b) shifting srcBufferOffset can break minBitstreamBufferOffsetAlignment
    // (256 for AV1 on NV) — Phase 3d.5 first attempt fixed alignment by
    // align-down + prefix rebase, but that dragged junk bytes into the
    // bitstream and driver mis-parsed them → black output.
    //
    // Correct approach: parser is now configured with 256-byte buffer offset
    // alignment (covers both H.264/H.265 16-byte and AV1 256-byte requirement).
    // Pass srcBufferOffset = bitstreamDataOffset directly (already 256-aligned),
    // srcBufferRange = bitstreamDataLen aligned UP to 256.  frameHeaderOffset =
    // OBU_FRAME header byte position within the bitstream (= scanForAv1FrameObu's
    // obuHeaderPos result).  tileOffsets = parser's absolute offsets within
    // the bitstream (no rebase).
    VkDeviceSize bsMaxSz = 0;
    uint8_t* bsMappedPtr = bsBuf->GetDataPtr(0, bsMaxSz);
    Av1FrameObuLocation obuLoc = {};
    bool obuFound = bsMappedPtr
        && scanForAv1FrameObu(bsMappedPtr + ppd->bitstreamDataOffset,
                              (size_t)ppd->bitstreamDataLen, &obuLoc);
    uint32_t obuShift = obuFound ? obuLoc.obuHeaderPos : 0;

    uint32_t numTiles = av1.khr_info.tileCount;
    if (numTiles > 64) numTiles = 64;

    VkVideoDecodeAV1PictureInfoKHR av1Pic = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR
    };
    av1Pic.pStdPictureInfo  = &av1.std_info;
    // frameHeaderOffset = OBU_FRAME header byte position (relative to
    // srcBufferOffset).  Driver parses obu_header + leb128 size + frame header
    // syntax starting from this byte.
    av1Pic.frameHeaderOffset = obuShift;
    av1Pic.tileCount        = numTiles;
    av1Pic.pTileOffsets     = av1.tileOffsets;
    av1Pic.pTileSizes       = av1.tileSizes;
    uint32_t frameHeaderOff = obuShift;  // for diag log compatibility
    // referenceNameSlotIndices[7]: AV1 ref_frame_idx[i] selects which of the
    // 8 DPB-buffer positions is the i-th reference (LAST/LAST2/.../ALTREF).
    // Convert to DPB slot indices via av1.pic_idx[] (-1 for inactive).
    for (int i = 0; i < STD_VIDEO_AV1_REFS_PER_FRAME; i++) {
        int rfi = av1.ref_frame_idx[i];
        if (rfi >= 0 && rfi < STD_VIDEO_AV1_NUM_REF_FRAMES
            && av1.pic_idx[rfi] >= 0) {
            av1Pic.referenceNameSlotIndices[i] = av1.pic_idx[rfi];
        } else {
            av1Pic.referenceNameSlotIndices[i] = -1;
        }
    }

    // §J.3.e.2.i.8 Phase 3d.5 — srcBufferOffset = bitstreamDataOffset directly.
    // Parser's 256-byte bufferOffsetAlignment guarantees 256-aligned (covers
    // AV1's strict requirement).  Range covers entire bitstream, aligned UP
    // to 256.  frameHeaderOffset already set above to OBU_FRAME header
    // position relative to srcBufferOffset.
    constexpr VkDeviceSize kBitstreamAlignment = 256;
    VkDeviceSize alignedRange = ((VkDeviceSize)ppd->bitstreamDataLen + (kBitstreamAlignment - 1)) & ~(kBitstreamAlignment - 1);

    VkVideoDecodeInfoKHR di = { VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR };
    di.pNext                = &av1Pic;
    di.srcBuffer            = bsBuf->GetBuffer();
    di.srcBufferOffset      = (VkDeviceSize)ppd->bitstreamDataOffset;
    di.srcBufferRange       = alignedRange;
    di.dstPictureResource   = setupResource;
    di.pSetupReferenceSlot  = &setupSlotInDecode;
    di.referenceSlotCount   = (uint32_t)activeRefCount;
    di.pReferenceSlots      = activeRefCount > 0 ? refSlots : nullptr;
    rt.CmdDecodeVideoKHR(m_DecodeCmdBuf, &di);

    // ── End video coding ──
    VkVideoEndCodingInfoKHR eci = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    rt.CmdEndVideoCodingKHR(m_DecodeCmdBuf, &eci);

    // ── Decode → SwUpload copy (codec-agnostic, identical to H.265 path) ──
    if (m_SwUploadImage != VK_NULL_HANDLE && m_SwImageWidth > 0 && m_SwImageHeight > 0) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdCopyImage = (PFN_vkCmdCopyImage)getDevPa(m_Device, "vkCmdCopyImage");

        VkImageMemoryBarrier2 preBars[2] = {};
        preBars[0].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        preBars[0].srcStageMask                = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        preBars[0].srcAccessMask               = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        preBars[0].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[0].dstAccessMask               = VK_ACCESS_2_TRANSFER_READ_BIT;
        preBars[0].oldLayout                   = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        preBars[0].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        preBars[0].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].image                       = curPic->image;
        preBars[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[0].subresourceRange.levelCount = 1;
        preBars[0].subresourceRange.baseArrayLayer = (uint32_t)slotIdx;
        preBars[0].subresourceRange.layerCount = 1;

        preBars[1].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        preBars[1].srcStageMask                = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        preBars[1].srcAccessMask               = 0;
        preBars[1].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[1].dstAccessMask               = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        preBars[1].oldLayout                   = m_SwImageLayoutInited
                                                   ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_UNDEFINED;
        preBars[1].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        preBars[1].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].image                       = m_SwUploadImage;
        preBars[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[1].subresourceRange.levelCount = 1;
        preBars[1].subresourceRange.layerCount = 1;

        VkDependencyInfo preDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.imageMemoryBarrierCount = 2;
        preDep.pImageMemoryBarriers    = preBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &preDep);

        VkImageCopy copyRegions[2] = {};
        copyRegions[0].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].srcSubresource.baseArrayLayer = (uint32_t)slotIdx;
        copyRegions[0].srcSubresource.layerCount     = 1;
        copyRegions[0].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].dstSubresource.baseArrayLayer = 0;
        copyRegions[0].dstSubresource.layerCount     = 1;
        copyRegions[0].extent                        = { (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight, 1 };
        copyRegions[1] = copyRegions[0];
        copyRegions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].extent = { (uint32_t)m_SwImageWidth / 2, (uint32_t)m_SwImageHeight / 2, 1 };
        if (pfnCmdCopyImage) {
            pfnCmdCopyImage(m_DecodeCmdBuf,
                curPic->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_SwUploadImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                2, copyRegions);
        }

        VkImageMemoryBarrier2 postBars[2] = {};
        postBars[0] = preBars[0];
        postBars[0].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        postBars[0].dstStageMask  = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        postBars[0].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR;
        postBars[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        postBars[0].newLayout     = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        postBars[1] = preBars[1];
        postBars[1].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        postBars[1].dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        postBars[1].dstAccessMask = 0;
        postBars[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postBars[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDependencyInfo postDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        postDep.imageMemoryBarrierCount = 2;
        postDep.pImageMemoryBarriers    = postBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &postDep);
        m_SwImageLayoutInited = true;
    }

    rt.EndCommandBuffer(m_DecodeCmdBuf);

    // ── Submit with timeline semaphore (Phase 1.5b — same as H.265 path) ──
    uint64_t signalVal = m_TimelineNext.fetch_add(1, std::memory_order_acq_rel);

    VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues    = &signalVal;

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &tssi;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_DecodeCmdBuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_TimelineSem;
    VkResult vr = rt.QueueSubmit(m_DecodeQueue, 1, &si, m_DecodeFence);
    if (vr == VK_SUCCESS) {
        m_LastDecodeValue.store(signalVal, std::memory_order_release);
        // §J.3.e.2.i.8 Phase 1.7 — pin this frame's bitstream buffer alive
        // until the next submitDecodeFrame*'s WaitForFences confirms the
        // GPU has drained NVDEC.  Replaces the per-frame use-after-free
        // race that the upstream parser's shared_ptr lifecycle introduced.
        m_PrevDecodeBsBuf = bsBuf;
    }
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.2b vkQueueSubmit(AV1) rc=%d — disabling native path", (int)vr);
        if (vr == VK_ERROR_DEVICE_LOST) m_DeviceLost.store(true, std::memory_order_release);
        m_DecodeSkipCount++;
        return false;
    }

    curPic->layoutInited = true;
    m_DpbSlotActive[slotIdx] = true;
    m_DecodeSubmitCount++;
    m_NewestDecodedSlot.store(slotIdx, std::memory_order_release);

    if (diagThisFrame) {
        // Phase 3d.4g/h — dump key std_info fields + ref mapping + OBU shift
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.2b AV1 diag #%llu — "
                    "slot=%d refs=%d showFrame=%d needsReset=%d "
                    "tiles=%u obuShift=%u srcRange=%llu fhOff=%u tile0OffAbs=%u tile0OffRel=%u tile0Sz=%u",
                    (unsigned long long)m_DecodeSubmitCount,
                    slotIdx, activeRefCount,
                    (int)av1.showFrame, (int)av1.needsSessionReset,
                    av1.khr_info.tileCount,
                    obuShift,
                    (unsigned long long)alignedRange,
                    frameHeaderOff,
                    av1.tileOffsets[0],
                    av1.tileOffsets[0],  // Phase 3d.5 — no rebase, parser's offset is final
                    av1.tileSizes[0]);
        // Phase 3d.4i — dump first 32 bytes of bitstream (post-shift) so we
        // can verify what the driver actually sees as "frame data".
        if (bsMappedPtr) {
            const uint8_t* p = bsMappedPtr + ppd->bitstreamDataOffset + obuShift;
            char hexBuf[256];
            int n = 0;
            for (int i = 0; i < 32 && i < (int)(ppd->bitstreamDataLen - obuShift); i++) {
                n += snprintf(hexBuf + n, sizeof(hexBuf) - n, "%02x ", p[i]);
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC]   AV1 post-shift first32: %s", hexBuf);
        }
        // std_info dump
        const auto& si = av1.std_info;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC]   AV1 std_info: type=%u OH=%u pri_ref=%u rf_flags=0x%02x "
                    "interpFil=%u TxMode=%u dq=%u dlf=%u skipMF=[%u,%u] coded_denom=%u "
                    "OH8=[%u,%u,%u,%u,%u,%u,%u,%u]",
                    (unsigned)si.frame_type, (unsigned)si.OrderHint,
                    (unsigned)si.primary_ref_frame, (unsigned)si.refresh_frame_flags,
                    (unsigned)si.interpolation_filter, (unsigned)si.TxMode,
                    (unsigned)si.delta_q_res, (unsigned)si.delta_lf_res,
                    (unsigned)si.SkipModeFrame[0], (unsigned)si.SkipModeFrame[1],
                    (unsigned)si.coded_denom,
                    (unsigned)si.OrderHints[0], (unsigned)si.OrderHints[1],
                    (unsigned)si.OrderHints[2], (unsigned)si.OrderHints[3],
                    (unsigned)si.OrderHints[4], (unsigned)si.OrderHints[5],
                    (unsigned)si.OrderHints[6], (unsigned)si.OrderHints[7]);
        // pic_idx + ref_frame_idx + referenceNameSlotIndices
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC]   AV1 picIdx=[%d,%d,%d,%d,%d,%d,%d,%d] "
                    "rfIdx=[%u,%u,%u,%u,%u,%u,%u] "
                    "rnSlot=[%d,%d,%d,%d,%d,%d,%d]",
                    av1.pic_idx[0], av1.pic_idx[1], av1.pic_idx[2], av1.pic_idx[3],
                    av1.pic_idx[4], av1.pic_idx[5], av1.pic_idx[6], av1.pic_idx[7],
                    av1.ref_frame_idx[0], av1.ref_frame_idx[1], av1.ref_frame_idx[2],
                    av1.ref_frame_idx[3], av1.ref_frame_idx[4], av1.ref_frame_idx[5],
                    av1.ref_frame_idx[6],
                    av1Pic.referenceNameSlotIndices[0], av1Pic.referenceNameSlotIndices[1],
                    av1Pic.referenceNameSlotIndices[2], av1Pic.referenceNameSlotIndices[3],
                    av1Pic.referenceNameSlotIndices[4], av1Pic.referenceNameSlotIndices[5],
                    av1Pic.referenceNameSlotIndices[6]);
    }
    if (m_DecodeSubmitCount == 1 || (m_DecodeSubmitCount % 60) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3b.2b AV1 decode submitted "
                    "#%llu (slot=%d refs=%d tiles=%u skipped=%llu)",
                    (unsigned long long)m_DecodeSubmitCount,
                    slotIdx, activeRefCount, av1.khr_info.tileCount,
                    (unsigned long long)m_DecodeSkipCount);
    }
    return true;
}

// =============================================================================
// Phase 2 — H.264 native decode submission body
// =============================================================================
//
// Modeled on submitDecodeFrameH265 with H.264-specific struct swaps:
//   * Reference collection iterates h264.dpb[16+1] directly (vs H.265's
//     RefPicSetStCurr*/After/LtCurr index-into-RefPics indirection).
//   * Std picture info is FLAT (no nested pointers like AV1 needed) — just
//     sps_id, pps_id, frame_num, idr_pic_id, PicOrderCnt[2], 6 1-bit flags.
//   * Std reference info is similarly flat — flags + FrameNum + PicOrderCnt[2].
//   * Slice offsets via parser's stream-marker pattern (same as H.265).
// Decode→SwUpload copy / barriers / submit / timeline sem are codec-agnostic.

bool VkFrucRenderer::submitDecodeFrameH264(VkParserPictureData* ppd)
{
    const auto& h264 = ppd->CodecSpecific.h264;

    // §J.3.e.2.i.8 Phase 2 — H.264 RESET policy.
    //
    // We DO NOT force a session RESET on H.264 intra pictures.  Unlike HEVC's
    // explicit IRAP/IDR pic flags, the parser-level intra_pic_flag fires on
    // every I-frame — and Sunshine's low-latency H.264 emits I-frames every
    // ~2 seconds for intra refresh / packet-loss recovery.  Resetting that
    // often (≈30× per minute) caused visible flicker in v1.3.274 because each
    // vkCmdControlVideoCodingKHR(RESET) drops the driver's internal DPB
    // tracking mid-stream → next P-frame references stale slot → glitch.
    //
    // Vulkan video H.264 doesn't need an explicit RESET on IDR: the driver
    // handles DPB clearing from the slice header itself, and our per-submit
    // VkVideoReferenceSlotInfoKHR list already reflects the parser's post-IDR
    // empty DPB.  RESET stays reserved for: (a) initial create (implicit) and
    // (b) recovery after VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR (TBD).

    bool diagThisFrame = (m_DecodeSubmitCount + m_DecodeSkipCount < 5)
                       || ppd->intra_pic_flag;

    auto* curPic = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(ppd->pCurrPic);
    if (!curPic || curPic->image == VK_NULL_HANDLE || curPic->view == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }
    int slotIdx = curPic->m_picIdx;

    // §J.3.e.2.i.8 Phase 2 — collect active reference DPB slots from H.264 dpb[].
    // Each entry: pPicBuf (VkFrucDpbPicture*), FrameIdx (frame_num/longTermFrameIdx),
    // is_long_term, not_existing, used_for_reference (0=unused/1=top/2=bot/3=both),
    // FieldOrderCnt[2].  Skip not_existing or unused entries; dedupe by slot.
    constexpr int kMaxRefs = 16;
    StdVideoDecodeH264ReferenceInfo refStdInfos[kMaxRefs] = {};
    VkVideoDecodeH264DpbSlotInfoKHR refH264Slots[kMaxRefs] = {};
    VkVideoPictureResourceInfoKHR   refResources[kMaxRefs] = {};
    VkVideoReferenceSlotInfoKHR     refSlots[kMaxRefs]     = {};
    int  activeRefCount = 0;
    bool seenSlot[VkFrucDecodeClient::kPicPoolSize] = {};

    constexpr int kDpbEntries = 16 + 1;  // matches dpb[16 + 1] layout
    for (int j = 0; j < kDpbEntries && activeRefCount < kMaxRefs; j++) {
        const auto& e = h264.dpb[j];
        if (e.not_existing || e.used_for_reference == 0) continue;
        auto* p = static_cast<VkFrucDecodeClient::VkFrucDpbPicture*>(e.pPicBuf);
        if (!p || p->image == VK_NULL_HANDLE || p->view == VK_NULL_HANDLE) continue;
        int slot = p->m_picIdx;
        if (slot < 0 || slot >= VkFrucDecodeClient::kPicPoolSize) continue;
        if (slot == slotIdx) continue;
        if (!m_DpbSlotActive[slot]) continue;
        if (seenSlot[slot]) continue;
        seenSlot[slot] = true;

        auto& sri = refStdInfos[activeRefCount];
        // §J.3.e.2.i.8 Phase 1.5c — H.264 PROGRESSIVE frame mode (the only
        // mode our session was created with; see createVideoSession's
        // VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR).  In Vulkan,
        // top_field_flag / bottom_field_flag describe a FIELD reference picture;
        // for a frame reference both flags are 0.  NV parser's used_for_reference
        // uses 0=unused / 1=top / 2=bot / 3=both (i.e. complete frame), but
        // mapping (val & 1) → top, (val & 2) → bottom literally turns
        // used_for_reference=3 into "this is BOTH a top field AND a bottom field
        // simultaneously" which validation reports as VUID-vkCmdDecodeVideoKHR-
        // pDecodeInfo-07260 ("reference picture is a field but session was not
        // created with interlaced frame support") and chains through 07267/07268
        // → eventual GPU DEVICE_LOST after 30-50s of accumulated mis-coded refs.
        // Sunshine streams are progressive, so always emit frame refs.  Pure
        // top-field or bottom-field references would only happen with interlaced
        // content; if we ever support that we'd also need to recreate the video
        // session with VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_*.
        sri.flags.top_field_flag              = 0;
        sri.flags.bottom_field_flag           = 0;
        sri.flags.used_for_long_term_reference = e.is_long_term ? 1 : 0;
        sri.flags.is_non_existing             = 0;  // already filtered above
        sri.FrameNum                          = (uint16_t)e.FrameIdx;
        sri.PicOrderCnt[0]                    = e.FieldOrderCnt[0];
        sri.PicOrderCnt[1]                    = e.FieldOrderCnt[1];

        auto& slotH264 = refH264Slots[activeRefCount];
        slotH264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        slotH264.pStdReferenceInfo = &sri;

        auto& res = refResources[activeRefCount];
        res.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        res.codedOffset      = { 0, 0 };
        res.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
        res.baseArrayLayer   = (uint32_t)slot;
        res.imageViewBinding = m_DpbDecodeArrayView;

        auto& s = refSlots[activeRefCount];
        s.sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        s.pNext            = &slotH264;
        s.slotIndex        = slot;
        s.pPictureResource = &res;

        activeRefCount++;
    }

    auto& bsBuf = ppd->bitstreamData;
    if (!bsBuf || bsBuf->GetBuffer() == VK_NULL_HANDLE) {
        m_DecodeSkipCount++;
        return false;
    }

    uint32_t markerCount = 0;
    const uint32_t* sliceOffsets = bsBuf->GetStreamMarkersPtr(ppd->firstSliceIndex, markerCount);
    if (!sliceOffsets || markerCount == 0) {
        m_DecodeSkipCount++;
        return false;
    }
    if (markerCount > ppd->numSlices) markerCount = ppd->numSlices;

    auto& rt = m_DecodeRtPfn;

    // §J.3.e.2.i.8 Phase 1.5c — gfx→decode timeline sem (replaces racey fence).
    uint64_t gfxWaitVal = m_LastGraphicsValue.load(std::memory_order_acquire);
    if (gfxWaitVal > 0 && m_GfxTimelineSem != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &m_GfxTimelineSem;
        wi.pValues        = &gfxWaitVal;
        rt.WaitSemaphores(m_Device, &wi, UINT64_MAX);
    }

    rt.WaitForFences(m_Device, 1, &m_DecodeFence, VK_TRUE, UINT64_MAX);
    rt.ResetFences(m_Device, 1, &m_DecodeFence);

    // §J.3.e.2.i.8 Phase 1.7 — see vkfruc.h m_PrevDecodeBsBuf comment.
    // GPU is now drained for the previous decode; safe to release the
    // bitstream buffer we pinned alive across that submit.
    m_PrevDecodeBsBuf.reset();

    rt.ResetCommandBuffer(m_DecodeCmdBuf, 0);

    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    rt.BeginCommandBuffer(m_DecodeCmdBuf, &cbi);

    // ── Layout transitions (single-image arrayLayers, identical to H.265 path) ──
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

        fillBarrier(curPic->image, (uint32_t)slotIdx, /*isRefRead*/false,
                    /*initFromUndefined*/true);
        for (int i = 0; i < activeRefCount; i++) {
            fillBarrier(curPic->image, (uint32_t)refSlots[i].slotIndex,
                        /*isRefRead*/true, /*initFromUndefined*/false);
        }

        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = (uint32_t)barrierCount;
        dep.pImageMemoryBarriers    = barriers;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &dep);
    }

    // ── Setup-ref slot (current picture) ──
    StdVideoDecodeH264ReferenceInfo stdSetupRef = {};
    stdSetupRef.flags.top_field_flag              = (ppd->field_pic_flag && !ppd->bottom_field_flag) ? 1 : 0;
    stdSetupRef.flags.bottom_field_flag           = (ppd->field_pic_flag && ppd->bottom_field_flag) ? 1 : 0;
    stdSetupRef.flags.used_for_long_term_reference = 0;  // newly-encoded pictures default short-term
    stdSetupRef.flags.is_non_existing             = 0;
    stdSetupRef.FrameNum                          = (uint16_t)h264.frame_num;
    stdSetupRef.PicOrderCnt[0]                    = h264.CurrFieldOrderCnt[0];
    stdSetupRef.PicOrderCnt[1]                    = h264.CurrFieldOrderCnt[1];

    VkVideoDecodeH264DpbSlotInfoKHR setupSlotH264 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR };
    setupSlotH264.pStdReferenceInfo = &stdSetupRef;

    VkVideoPictureResourceInfoKHR setupResource = { VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
    setupResource.codedOffset      = { 0, 0 };
    setupResource.codedExtent      = { (uint32_t)m_DpbAlignedW, (uint32_t)m_DpbAlignedH };
    setupResource.baseArrayLayer   = (uint32_t)slotIdx;
    setupResource.imageViewBinding = m_DpbDecodeArrayView;

    VkVideoReferenceSlotInfoKHR setupSlotInBegin = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
    setupSlotInBegin.pNext            = &setupSlotH264;
    setupSlotInBegin.slotIndex        = -1;
    setupSlotInBegin.pPictureResource = &setupResource;

    VkVideoReferenceSlotInfoKHR setupSlotInDecode = setupSlotInBegin;
    setupSlotInDecode.slotIndex = slotIdx;

    // ── Begin video coding ──
    VkVideoReferenceSlotInfoKHR allCodingSlots[1 + kMaxRefs];
    allCodingSlots[0] = setupSlotInBegin;
    for (int i = 0; i < activeRefCount; i++) allCodingSlots[1 + i] = refSlots[i];

    VkVideoBeginCodingInfoKHR bci = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    bci.videoSession           = m_VideoSession;
    bci.videoSessionParameters = m_VideoSessionParams;
    bci.referenceSlotCount     = (uint32_t)(1 + activeRefCount);
    bci.pReferenceSlots        = allCodingSlots;
    rt.CmdBeginVideoCodingKHR(m_DecodeCmdBuf, &bci);

    if (m_DecodeNeedsReset) {
        VkVideoCodingControlInfoKHR cci = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR };
        cci.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        rt.CmdControlVideoCodingKHR(m_DecodeCmdBuf, &cci);
        m_DecodeNeedsReset = false;
        for (int i = 0; i < VkFrucDecodeClient::kPicPoolSize; i++) m_DpbSlotActive[i] = false;
    }

    // ── Decode ──
    StdVideoDecodeH264PictureInfo stdPicInfo = {};
    // intra_pic_flag is generic (not in h264 union); is_intra in std flags follows.
    stdPicInfo.flags.is_intra              = ppd->intra_pic_flag ? 1 : 0;
    stdPicInfo.flags.IdrPicFlag            = ppd->intra_pic_flag ? 1 : 0;  // IDR ≈ intra in our streaming
    stdPicInfo.flags.is_reference          = ppd->ref_pic_flag ? 1 : 0;
    stdPicInfo.flags.field_pic_flag        = ppd->field_pic_flag ? 1 : 0;
    stdPicInfo.flags.bottom_field_flag     = ppd->bottom_field_flag ? 1 : 0;
    stdPicInfo.flags.complementary_field_pair = 0;
    stdPicInfo.seq_parameter_set_id        = h264.seq_parameter_set_id;
    stdPicInfo.pic_parameter_set_id        = h264.pic_parameter_set_id;
    stdPicInfo.frame_num                   = (uint16_t)h264.frame_num;
    stdPicInfo.idr_pic_id                  = 0;  // parser doesn't expose; driver
                                                 // typically tolerates 0 for streaming
    stdPicInfo.PicOrderCnt[0]              = h264.CurrFieldOrderCnt[0];
    stdPicInfo.PicOrderCnt[1]              = h264.CurrFieldOrderCnt[1];

    VkVideoDecodeH264PictureInfoKHR h264Pic = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR };
    h264Pic.pStdPictureInfo = &stdPicInfo;
    h264Pic.sliceCount      = markerCount;
    h264Pic.pSliceOffsets   = sliceOffsets;

    VkDeviceSize alignedRange = (ppd->bitstreamDataLen + 255u) & ~VkDeviceSize(255);

    VkVideoDecodeInfoKHR di = { VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR };
    di.pNext                = &h264Pic;
    di.srcBuffer            = bsBuf->GetBuffer();
    di.srcBufferOffset      = ppd->bitstreamDataOffset;
    di.srcBufferRange       = alignedRange;
    di.dstPictureResource   = setupResource;
    di.pSetupReferenceSlot  = &setupSlotInDecode;
    di.referenceSlotCount   = (uint32_t)activeRefCount;
    di.pReferenceSlots      = activeRefCount > 0 ? refSlots : nullptr;
    rt.CmdDecodeVideoKHR(m_DecodeCmdBuf, &di);

    VkVideoEndCodingInfoKHR eci = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    rt.CmdEndVideoCodingKHR(m_DecodeCmdBuf, &eci);

    // ── Decode → SwUpload copy (codec-agnostic, identical to H.265 path) ──
    if (m_SwUploadImage != VK_NULL_HANDLE && m_SwImageWidth > 0 && m_SwImageHeight > 0) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdCopyImage = (PFN_vkCmdCopyImage)getDevPa(m_Device, "vkCmdCopyImage");

        VkImageMemoryBarrier2 preBars[2] = {};
        preBars[0].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        preBars[0].srcStageMask                = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        preBars[0].srcAccessMask               = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        preBars[0].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[0].dstAccessMask               = VK_ACCESS_2_TRANSFER_READ_BIT;
        preBars[0].oldLayout                   = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        preBars[0].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        preBars[0].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].image                       = curPic->image;
        preBars[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[0].subresourceRange.levelCount = 1;
        preBars[0].subresourceRange.baseArrayLayer = (uint32_t)slotIdx;
        preBars[0].subresourceRange.layerCount = 1;

        preBars[1].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        preBars[1].srcStageMask                = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        preBars[1].srcAccessMask               = 0;
        preBars[1].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[1].dstAccessMask               = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        preBars[1].oldLayout                   = m_SwImageLayoutInited
                                                   ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_UNDEFINED;
        preBars[1].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        preBars[1].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].image                       = m_SwUploadImage;
        preBars[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[1].subresourceRange.levelCount = 1;
        preBars[1].subresourceRange.layerCount = 1;

        VkDependencyInfo preDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.imageMemoryBarrierCount = 2;
        preDep.pImageMemoryBarriers    = preBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &preDep);

        VkImageCopy copyRegions[2] = {};
        copyRegions[0].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].srcSubresource.baseArrayLayer = (uint32_t)slotIdx;
        copyRegions[0].srcSubresource.layerCount     = 1;
        copyRegions[0].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].dstSubresource.baseArrayLayer = 0;
        copyRegions[0].dstSubresource.layerCount     = 1;
        copyRegions[0].extent                        = { (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight, 1 };
        copyRegions[1] = copyRegions[0];
        copyRegions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].extent = { (uint32_t)m_SwImageWidth / 2, (uint32_t)m_SwImageHeight / 2, 1 };
        if (pfnCmdCopyImage) {
            pfnCmdCopyImage(m_DecodeCmdBuf,
                curPic->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_SwUploadImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                2, copyRegions);
        }

        VkImageMemoryBarrier2 postBars[2] = {};
        postBars[0] = preBars[0];
        postBars[0].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        postBars[0].dstStageMask  = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        postBars[0].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR;
        postBars[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        postBars[0].newLayout     = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        postBars[1] = preBars[1];
        postBars[1].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        postBars[1].dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        postBars[1].dstAccessMask = 0;
        postBars[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postBars[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDependencyInfo postDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        postDep.imageMemoryBarrierCount = 2;
        postDep.pImageMemoryBarriers    = postBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &postDep);
        m_SwImageLayoutInited = true;
    }

    rt.EndCommandBuffer(m_DecodeCmdBuf);

    uint64_t signalVal = m_TimelineNext.fetch_add(1, std::memory_order_acq_rel);

    VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues    = &signalVal;

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &tssi;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_DecodeCmdBuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_TimelineSem;
    VkResult vr = rt.QueueSubmit(m_DecodeQueue, 1, &si, m_DecodeFence);
    if (vr == VK_SUCCESS) {
        m_LastDecodeValue.store(signalVal, std::memory_order_release);
        // §J.3.e.2.i.8 Phase 1.7 — pin this frame's bitstream buffer alive
        // until the next submitDecodeFrame*'s WaitForFences confirms the
        // GPU has drained NVDEC.  Replaces the per-frame use-after-free
        // race that the upstream parser's shared_ptr lifecycle introduced.
        m_PrevDecodeBsBuf = bsBuf;
    }
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 vkQueueSubmit(H.264) rc=%d — disabling native path", (int)vr);
        if (vr == VK_ERROR_DEVICE_LOST) m_DeviceLost.store(true, std::memory_order_release);
        m_DecodeSkipCount++;
        return false;
    }

    curPic->layoutInited = true;
    m_DpbSlotActive[slotIdx] = true;
    m_DecodeSubmitCount++;
    m_NewestDecodedSlot.store(slotIdx, std::memory_order_release);

    if (diagThisFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 H.264 diag #%llu — "
                    "slot=%d refs=%d intra=%d field=%d frame_num=%u POC=[%d,%d] slices=%u",
                    (unsigned long long)m_DecodeSubmitCount,
                    slotIdx, activeRefCount,
                    (int)ppd->intra_pic_flag, (int)ppd->field_pic_flag,
                    (unsigned)h264.frame_num,
                    h264.CurrFieldOrderCnt[0], h264.CurrFieldOrderCnt[1],
                    markerCount);
    }
    if (m_DecodeSubmitCount == 1 || (m_DecodeSubmitCount % 60) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2 H.264 decode submitted "
                    "#%llu (slot=%d refs=%d slices=%u skipped=%llu)",
                    (unsigned long long)m_DecodeSubmitCount,
                    slotIdx, activeRefCount, markerCount,
                    (unsigned long long)m_DecodeSkipCount);
    }
    return true;
}


// =============================================================================
// Phase 1.3d / 1.3d.2 — H.265 native decode submission body
// =============================================================================
//
// Renamed from submitDecodeFrame in Phase 3b.2a.  Pre-conditions (m_DpbReady,
// PFN load, valid video session) are guaranteed by the dispatcher; this
// helper goes straight into recording.

bool VkFrucRenderer::submitDecodeFrameH265(VkParserPictureData* ppd)
{
    const auto& hevc = ppd->CodecSpecific.hevc;

    // §J.3.e.2.i.8 Phase 1.3d.2.c — Catch ANY IRAP (random-access picture:
    // IDR / CRA / BLA) after the first decode, force session RESET so the
    // DPB tracking starts fresh.  Initial trigger only on IdrPicFlag missed
    // mid-stream IRAPs that Sunshine emits on packet loss / GOP refresh.
    if ((hevc.IrapPicFlag || hevc.IdrPicFlag) && m_DecodeSubmitCount > 0) {
        m_DecodeNeedsReset = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d.2.c — IRAP/IDR after %llu submits "
                    "(IRAP=%d IDR=%d) — queueing session RESET",
                    (unsigned long long)m_DecodeSubmitCount,
                    (int)hevc.IrapPicFlag, (int)hevc.IdrPicFlag);
    }

    // §J.3.e.2.i.8 Phase 1.3d.2.c — first ~5 frames + every IRAP get diag log
    // for crash forensics (slot, ref count, IRAP/IDR/POC).  Periodic frames
    // remain on the existing 60-frame counter to avoid log flood.
    bool diagThisFrame = (m_DecodeSubmitCount + m_DecodeSkipCount < 5)
                       || hevc.IrapPicFlag || hevc.IdrPicFlag;

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
        // 2D_ARRAY decode view + per-slot layer index (§J.3.e.2.i.8 Phase 1.3d.2.d).
        res.baseArrayLayer   = (uint32_t)slot;
        res.imageViewBinding = m_DpbDecodeArrayView;

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

    // §J.3.e.2.i.8 Phase 1.4 proper — wait for prior graphics-queue submit
    // (which samples m_SwUploadImage) to finish BEFORE we start recording
    // the next decode's m_SwUploadImage overwrite.  This eliminates the
    // cross-queue race that was killing v1.3.241 after ~300-480 frames.
    // §J.3.e.2.i.8 Phase 1.5c — gfx→decode timeline sem (replaces racey fence).
    uint64_t gfxWaitVal = m_LastGraphicsValue.load(std::memory_order_acquire);
    if (gfxWaitVal > 0 && m_GfxTimelineSem != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &m_GfxTimelineSem;
        wi.pValues        = &gfxWaitVal;
        rt.WaitSemaphores(m_Device, &wi, UINT64_MAX);
    }

    // Wait for previous decode submission to complete (single cmd buffer pattern).
    rt.WaitForFences(m_Device, 1, &m_DecodeFence, VK_TRUE, UINT64_MAX);
    rt.ResetFences(m_Device, 1, &m_DecodeFence);

    // §J.3.e.2.i.8 Phase 1.7 — previous decode is now GPU-complete.  Drop
    // our hold on the previous frame's bitstream buffer; if no other ref
    // exists, this fires the destructor → vkDestroyBuffer + vkFreeMemory,
    // which is now safe (NVDEC can't still be reading it).  See vkfruc.h
    // m_PrevDecodeBsBuf comment for the page-fault that motivated this.
    m_PrevDecodeBsBuf.reset();

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

        // Setup/dst layer barrier — always treat the destination layer as
        // fresh (UNDEFINED → DPB).  When a slot is reused after being a
        // reference for older frames, NV's HEVC decoder appears to reject
        // a same-layout DPB→DPB transition for write; transitioning from
        // UNDEFINED discards prior content, which is what we want anyway
        // (we're about to overwrite the layer with a new picture).
        fillBarrier(curPic->image, (uint32_t)slotIdx, /*isRefRead*/false,
                    /*initFromUndefined*/true);

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
    // §J.3.e.2.i.8 Phase 1.3d.2.d — point at the 2D_ARRAY decode view + use
    // baseArrayLayer to select the slot's layer (NV vk_video_samples pattern).
    setupResource.baseArrayLayer   = (uint32_t)slotIdx;
    setupResource.imageViewBinding = m_DpbDecodeArrayView;

    // Spec: in BeginCoding's pReferenceSlots, an entry with slotIndex=-1 means
    // "this slot is being (re)activated in this scope".  ALWAYS use -1 here:
    // even when the slot was previously active, a new picture is being
    // installed and the driver treats it as a re-setup.  Earlier code branched
    // (active ? N : -1) but NV-HEVC device-lost'd around frame 60, which
    // coincided with slot reuse.  Forcing -1 unconditionally avoids that path.
    // DecodeInfo.pSetupReferenceSlot still carries the actual slot index.
    VkVideoReferenceSlotInfoKHR setupSlotInBegin = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR };
    setupSlotInBegin.pNext            = &setupSlotH265;
    setupSlotInBegin.slotIndex        = -1;
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

    // §J.3.e.2.i.8 Phase 1.4 — copy decoded layer to m_SwUploadImage in the
    // SAME decode-queue cmd buffer.  Sync2 barriers because video-decode
    // pipeline stage only exists in the _2 form.
    if (m_SwUploadImage != VK_NULL_HANDLE && m_SwImageWidth > 0 && m_SwImageHeight > 0) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdCopyImage = (PFN_vkCmdCopyImage)getDevPa(m_Device, "vkCmdCopyImage");

        // Pre-copy: DPB layer → TRANSFER_SRC, m_SwUploadImage → TRANSFER_DST
        VkImageMemoryBarrier2 preBars[2] = {};
        preBars[0].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        preBars[0].srcStageMask                = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        preBars[0].srcAccessMask               = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        preBars[0].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[0].dstAccessMask               = VK_ACCESS_2_TRANSFER_READ_BIT;
        preBars[0].oldLayout                   = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        preBars[0].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        preBars[0].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[0].image                       = curPic->image;
        preBars[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[0].subresourceRange.levelCount = 1;
        preBars[0].subresourceRange.baseArrayLayer = (uint32_t)slotIdx;
        preBars[0].subresourceRange.layerCount = 1;

        preBars[1].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        // §J.3.e.2.i.8 Phase 1.5 — decode queue family doesn't support
        // FRAGMENT_SHADER stage. Use TOP_OF_PIPE + 0 access for the "release"
        // side; cross-queue sync to graphics done via CPU-side fence wait.
        preBars[1].srcStageMask                = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        preBars[1].srcAccessMask               = 0;
        preBars[1].dstStageMask                = VK_PIPELINE_STAGE_2_COPY_BIT;
        preBars[1].dstAccessMask               = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        preBars[1].oldLayout                   = m_SwImageLayoutInited
                                                   ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_UNDEFINED;
        preBars[1].newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        preBars[1].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        preBars[1].image                       = m_SwUploadImage;
        preBars[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        preBars[1].subresourceRange.levelCount = 1;
        preBars[1].subresourceRange.layerCount = 1;

        VkDependencyInfo preDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.imageMemoryBarrierCount = 2;
        preDep.pImageMemoryBarriers    = preBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &preDep);

        // Two-plane copy: Y (PLANE_0) + UV (PLANE_1, half-resolution chroma 4:2:0).
        VkImageCopy copyRegions[2] = {};
        copyRegions[0].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].srcSubresource.baseArrayLayer = (uint32_t)slotIdx;
        copyRegions[0].srcSubresource.layerCount     = 1;
        copyRegions[0].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_PLANE_0_BIT;
        copyRegions[0].dstSubresource.baseArrayLayer = 0;
        copyRegions[0].dstSubresource.layerCount     = 1;
        copyRegions[0].extent                        = { (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight, 1 };
        copyRegions[1] = copyRegions[0];
        copyRegions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copyRegions[1].extent = { (uint32_t)m_SwImageWidth / 2, (uint32_t)m_SwImageHeight / 2, 1 };
        if (pfnCmdCopyImage) {
            pfnCmdCopyImage(m_DecodeCmdBuf,
                curPic->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_SwUploadImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                2, copyRegions);
        }

        // Post-copy: DPB → DPB (so next decode reads it as ref OK), m_SwUploadImage → SHADER_READ_ONLY
        VkImageMemoryBarrier2 postBars[2] = {};
        postBars[0] = preBars[0];
        postBars[0].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        postBars[0].dstStageMask  = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        postBars[0].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR;
        postBars[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        postBars[0].newLayout     = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        postBars[1] = preBars[1];
        postBars[1].srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        postBars[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // §J.3.e.2.i.8 Phase 1.5 — BOTTOM_OF_PIPE + 0 access, graphics queue
        // does its own acquire when sampling (CONCURRENT image — no QF transfer).
        postBars[1].dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        postBars[1].dstAccessMask = 0;
        postBars[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postBars[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDependencyInfo postDep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        postDep.imageMemoryBarrierCount = 2;
        postDep.pImageMemoryBarriers    = postBars;
        rt.CmdPipelineBarrier2(m_DecodeCmdBuf, &postDep);
        m_SwImageLayoutInited = true;
    }

    rt.EndCommandBuffer(m_DecodeCmdBuf);

    // §J.3.e.2.i.8 Phase 1.5b — signal timeline semaphore so graphics queue
    // can wait on it in renderFrameSw.  Allocate a monotonically increasing
    // value (m_TimelineNext) and publish it via m_LastDecodeValue so the
    // graphics submit knows what to wait for.  Allocate the value BEFORE
    // QueueSubmit so the wait→signal happens-before relationship is clear
    // even if graphics reads m_LastDecodeValue before our QueueSubmit returns.
    uint64_t signalVal = m_TimelineNext.fetch_add(1, std::memory_order_acq_rel);

    VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues    = &signalVal;

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &tssi;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_DecodeCmdBuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_TimelineSem;
    VkResult vr = rt.QueueSubmit(m_DecodeQueue, 1, &si, m_DecodeFence);
    if (vr == VK_SUCCESS) {
        m_LastDecodeValue.store(signalVal, std::memory_order_release);
        // §J.3.e.2.i.8 Phase 1.7 — pin this frame's bitstream buffer alive
        // until the next submitDecodeFrame*'s WaitForFences confirms the
        // GPU has drained NVDEC.  Replaces the per-frame use-after-free
        // race that the upstream parser's shared_ptr lifecycle introduced.
        m_PrevDecodeBsBuf = bsBuf;
    }
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d vkQueueSubmit rc=%d — disabling native path", (int)vr);
        if (vr == VK_ERROR_DEVICE_LOST) m_DeviceLost.store(true, std::memory_order_release);
        m_DecodeSkipCount++;
        return false;
    }

    // §J.3.e.2.i.8 Phase 1.4 proper (v1.3.235→242 transition):
    // vkDeviceWaitIdle removed.  Cross-queue sync now handled by:
    //  - Decode submit START waits on m_LastGraphicsFence (graphics finished
    //    sampling m_SwUploadImage before we overwrite it).
    //  - Decode submit END signals m_DecodeFence (already), so the next
    //    submitDecodeFrame's WaitForFences below blocks until prior decode
    //    finished writing.  Graphics' own per-slot fences guard their reads.
    // The decode-queue cmd buffer's sync2 image barriers (around vkCmdCopyImage)
    // handle the WITHIN-queue write→write ordering on m_SwUploadImage.

    curPic->layoutInited = true;
    m_DpbSlotActive[slotIdx] = true;   // slot is now usable as a reference
    m_DecodeSubmitCount++;
    // §J.3.e.2.i.8 Phase 1.4 — publish the freshly decoded slot for renderFrameSw
    // to copy into m_SwUploadImage.  Atomic store ensures cross-thread visibility
    // (decode runs on submitNativeDecodeUnit thread, render on Pacer thread).
    m_NewestDecodedSlot.store(slotIdx, std::memory_order_release);
    if (diagThisFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d diag #%llu — "
                    "slot=%d refs=%d IRAP=%d IDR=%d POC=%d slices=%u "
                    "active{0..6}=%d%d%d%d%d%d%d",
                    (unsigned long long)m_DecodeSubmitCount,
                    slotIdx, activeRefCount,
                    (int)hevc.IrapPicFlag, (int)hevc.IdrPicFlag,
                    hevc.CurrPicOrderCntVal, markerCount,
                    (int)m_DpbSlotActive[0], (int)m_DpbSlotActive[1],
                    (int)m_DpbSlotActive[2], (int)m_DpbSlotActive[3],
                    (int)m_DpbSlotActive[4], (int)m_DpbSlotActive[5],
                    (int)m_DpbSlotActive[6]);
    }
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

    // §J.3.e.2.i.8 Phase 3d.4c — codec-aware DPB image extent alignment.
    // HEVC CTU max=64; AV1 super-block max=128 (NV driver advertises
    // min=128x128 for AV1 in the per-codec capability probe at startup,
    // which suggests pictureAccessGranularity is also 128 for that codec).
    // Mismatched alignment → decoder writes beyond/inside DPB image edges
    // and the resulting NV12 has uninitialized regions sampled as grey
    // (Y=0,U=V=128) on alternating frames depending on slot reuse pattern.
    auto alignUp = [](int x, int a) { return (x + a - 1) & ~(a - 1); };
    int blockAlign = (m_VideoCodec & VIDEO_FORMAT_MASK_AV1) ? 128 : 64;
    int alignedW = alignUp(width, blockAlign);
    int alignedH = alignUp(height, blockAlign);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 3d.4c DPB pool extent "
                "%dx%d aligned to %d-block → %dx%d",
                width, height, blockAlign, alignedW, alignedH);

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
    // §J.3.e.2.i.8 Phase 1.4 — graphics queue family also reads this image
    // (vkCmdCopyImage in renderFrameSw), so use CONCURRENT sharing across
    // [graphics, decode] QFs (avoids ownership transfer barriers).  Add
    // TRANSFER_SRC bit so the graphics QF can vkCmdCopyImage from any layer.
    uint32_t qfs[2] = { m_QueueFamily, m_DecodeQueueFamily };
    bool concurrent = (m_QueueFamily != m_DecodeQueueFamily);

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext                 = &profileList;
    ici.imageType             = VK_IMAGE_TYPE_2D;
    ici.format                = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ici.extent                = { (uint32_t)alignedW, (uint32_t)alignedH, 1 };
    ici.mipLevels             = 1;
    ici.arrayLayers           = (uint32_t)VkFrucDecodeClient::kPicPoolSize;
    ici.samples               = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling                = VK_IMAGE_TILING_OPTIMAL;
    ici.usage                 = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR
                              | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR
                              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode           = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = concurrent ? 2u : 0u;
    ici.pQueueFamilyIndices   = concurrent ? qfs : nullptr;
    ici.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

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
    // Used by Phase 1.4 graphics sampling later (each will pair with its own
    // VkSamplerYcbcrConversion).  Decode itself uses m_DpbDecodeArrayView below.
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

    // §J.3.e.2.i.8 Phase 1.3d.2.d — single 2D_ARRAY view used by video-decode.
    // Matches NV vk_video_samples pattern; submit uses
    // VkVideoPictureResourceInfoKHR.imageViewBinding=this + baseArrayLayer=N.
    {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image                           = m_DpbSharedImage;
        vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format                          = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.components                      = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount     = (uint32_t)VkFrucDecodeClient::kPicPoolSize;
        vr = pfnCreateImageView(m_Device, &vci, nullptr, &m_DpbDecodeArrayView);
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3d.2.d vkCreateImageView (decode 2D_ARRAY) rc=%d",
                         (int)vr);
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
    if (m_DpbDecodeArrayView != VK_NULL_HANDLE && pfnDestroyImageView)
        pfnDestroyImageView(m_Device, m_DpbDecodeArrayView, nullptr);
    if (m_DpbSharedImage != VK_NULL_HANDLE && pfnDestroyImage)
        pfnDestroyImage(m_Device, m_DpbSharedImage, nullptr);
    if (m_DpbSharedMem != VK_NULL_HANDLE && pfnFreeMem)
        pfnFreeMem(m_Device, m_DpbSharedMem, nullptr);
    m_DpbDecodeArrayView = VK_NULL_HANDLE;
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
    // §J.3.e.2.i.8 Phase 1 (H.265) + Phase 2 (H.264) + Phase 3a (AV1).  VP9
    // still gated out from the parser library build (VIPLESTREAM_NVPARSER_NO_VP9
    // in nvvideoparser.pro); attempting that codec would hit the defensive
    // null-shared_ptr bail in CreateVulkanVideoDecodeParser and fail cleanly
    // with VK_ERROR_FEATURE_NOT_PRESENT.  We bail early here for codecs we
    // know aren't ported.
    const bool isH264 = (m_VideoCodec & VIDEO_FORMAT_MASK_H264) != 0;
    const bool isH265 = (m_VideoCodec & VIDEO_FORMAT_MASK_H265) != 0;
    const bool isAv1  = (m_VideoCodec & VIDEO_FORMAT_MASK_AV1)  != 0;
    if (!isH264 && !isH265 && !isAv1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createNvVideoParser SKIPPED — "
                    "current stream codec mask=0x%x is none of H.264 / H.265 / AV1",
                    m_VideoCodec);
        return false;  // caller logs warning + falls back to legacy NAL-counter path
    }

    auto pimpl = new NvParserPimpl();
    pimpl->client = std::make_shared<VkFrucDecodeClient>(this);

    VkExtensionProperties stdHeaderVer = {};
    VkVideoCodecOperationFlagBitsKHR codecOp;
    if (isH264) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    } else if (isH265) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else if (isAv1) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    } else {
        // Unreachable due to early bail above — keep for future codec phases.
        delete pimpl;
        return false;
    }

    VkParserInitDecodeParameters initParams = {};
    initParams.interfaceVersion          = NV_VULKAN_VIDEO_PARSER_API_VERSION;
    initParams.pClient                   = pimpl->client.get();
    initParams.defaultMinBufferSize      = 1024 * 1024;
    // §J.3.e.2.i.8 Phase 3d.5 — parser-level alignment must match Vulkan
    // video session's minBitstreamBufferOffsetAlignment (256 for AV1 on NV).
    // 64 was OK for H.264/H.265 but for AV1 caused align-down at submit time,
    // dragging junk prefix bytes into the OBU stream → driver mis-parses →
    // black screen (Phase 3d.5).  256 covers all 3 codecs safely.
    initParams.bufferOffsetAlignment     = 256;
    initParams.bufferSizeAlignment       = 256;
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
