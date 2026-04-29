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

// §J.3.e.2.i.8 Phase 1.1c — minimal CPU-side bitstream buffer.
//
// VulkanBitstreamBuffer 19 個 pure virtual 的 minimal impl: 用 std::vector
// 保 NAL 拷貝 + 各種 stub. VkBuffer/VkDeviceMemory 全 NULL_HANDLE，因為
// 此 phase parser 只 ParseByteStream (不 submit decode).  Phase 1.3 把
// 這個換成真的 vkCreateBuffer + vkAllocateMemory + vkMapMemory.
//
// 寫在 anonymous namespace 隔離 link visibility.
namespace {
class VkFrucCpuBitstreamBuffer : public VulkanBitstreamBuffer {
public:
    VkFrucCpuBitstreamBuffer(size_t size, size_t offsetAlign, size_t sizeAlign)
        : m_buf(size), m_offsetAlign(offsetAlign), m_sizeAlign(sizeAlign) {}

    VkDeviceSize GetMaxSize() const override { return m_buf.size(); }
    VkDeviceSize GetOffsetAlignment() const override { return m_offsetAlign; }
    VkDeviceSize GetSizeAlignment()   const override { return m_sizeAlign;   }

    VkDeviceSize Resize(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset) override {
        if (newSize <= m_buf.size()) return m_buf.size();
        std::vector<uint8_t> newBuf(newSize, 0);
        if (copySize) memcpy(newBuf.data(), m_buf.data() + copyOffset, copySize);
        m_buf.swap(newBuf);
        return m_buf.size();
    }
    VkDeviceSize Clone(VkDeviceSize newSize, VkDeviceSize copySize, VkDeviceSize copyOffset,
                       VkSharedBaseObj<VulkanBitstreamBuffer>& vulkanBitstreamBuffer) override {
        auto cl = std::make_shared<VkFrucCpuBitstreamBuffer>(newSize, m_offsetAlign, m_sizeAlign);
        if (copySize && copyOffset + copySize <= m_buf.size())
            memcpy(cl->m_buf.data(), m_buf.data() + copyOffset, copySize);
        vulkanBitstreamBuffer = cl;
        return cl->GetMaxSize();
    }

    int64_t MemsetData(uint32_t value, VkDeviceSize offset, VkDeviceSize size) override {
        if (offset + size > m_buf.size()) return -1;
        memset(m_buf.data() + offset, (int)value, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataToBuffer(uint8_t* dst, VkDeviceSize dstOff, VkDeviceSize srcOff, VkDeviceSize size) const override {
        if (srcOff + size > m_buf.size()) return -1;
        memcpy(dst + dstOff, m_buf.data() + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataToBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& dst, VkDeviceSize dstOff,
                              VkDeviceSize srcOff, VkDeviceSize size) const override {
        if (!dst || srcOff + size > m_buf.size()) return -1;
        VkDeviceSize maxSz = 0;
        uint8_t* dstPtr = dst->GetDataPtr(dstOff, maxSz);
        if (!dstPtr || size > maxSz) return -1;
        memcpy(dstPtr, m_buf.data() + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataFromBuffer(const uint8_t* src, VkDeviceSize srcOff, VkDeviceSize dstOff, VkDeviceSize size) override {
        if (dstOff + size > m_buf.size()) return -1;
        memcpy(m_buf.data() + dstOff, src + srcOff, (size_t)size);
        return (int64_t)size;
    }
    int64_t CopyDataFromBuffer(const VkSharedBaseObj<VulkanBitstreamBuffer>& src, VkDeviceSize srcOff,
                                VkDeviceSize dstOff, VkDeviceSize size) override {
        if (!src || dstOff + size > m_buf.size()) return -1;
        VkDeviceSize maxSz = 0;
        const uint8_t* srcPtr = src->GetReadOnlyDataPtr(srcOff, maxSz);
        if (!srcPtr || size > maxSz) return -1;
        memcpy(m_buf.data() + dstOff, srcPtr, (size_t)size);
        return (int64_t)size;
    }
    uint8_t* GetDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) override {
        if (offset >= m_buf.size()) { maxSize = 0; return nullptr; }
        maxSize = m_buf.size() - offset;
        return m_buf.data() + offset;
    }
    const uint8_t* GetReadOnlyDataPtr(VkDeviceSize offset, VkDeviceSize& maxSize) const override {
        if (offset >= m_buf.size()) { maxSize = 0; return nullptr; }
        maxSize = m_buf.size() - offset;
        return m_buf.data() + offset;
    }
    void FlushRange(VkDeviceSize, VkDeviceSize) const override {}      // CPU-only, noop
    void InvalidateRange(VkDeviceSize, VkDeviceSize) const override {} // CPU-only, noop
    VkBuffer       GetBuffer()       const override { return VK_NULL_HANDLE; }  // Phase 1.3 fix
    VkDeviceMemory GetDeviceMemory() const override { return VK_NULL_HANDLE; }  // Phase 1.3 fix

    uint32_t AddStreamMarker(uint32_t off) override { m_markers.push_back(off); return (uint32_t)m_markers.size() - 1; }
    uint32_t SetStreamMarker(uint32_t off, uint32_t idx) override {
        if (idx >= m_markers.size()) m_markers.resize(idx + 1, 0);
        m_markers[idx] = off;
        return idx;
    }
    uint32_t GetStreamMarker(uint32_t idx) const override { return idx < m_markers.size() ? m_markers[idx] : 0; }
    uint32_t GetStreamMarkersCount() const override { return (uint32_t)m_markers.size(); }
    const uint32_t* GetStreamMarkersPtr(uint32_t startIdx, uint32_t& maxCount) const override {
        if (startIdx >= m_markers.size()) { maxCount = 0; return nullptr; }
        maxCount = (uint32_t)m_markers.size() - startIdx;
        return m_markers.data() + startIdx;
    }
    uint32_t ResetStreamMarkers() override { m_markers.clear(); return 0; }

private:
    std::vector<uint8_t>  m_buf;
    std::vector<uint32_t> m_markers;
    size_t m_offsetAlign;
    size_t m_sizeAlign;
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
    // Phase 1.3 — 若 stream 維度變了, recreate VkVideoSession.
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
    }

    // Phase 1.3 — 在這裡呼叫 vkCmdDecodeVideoKHR.
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
    // §J.3.e.2.i.8 Phase 1.1c — 配 CPU-only bitstream buffer 給 parser.
    // Parser 在 ParseByteStream 把 NAL bytes 拷進來 + walks bytes 解出
    // SPS/PPS/slice headers + 觸發我們的 callback. 此 phase 不 submit
    // decode → VkBuffer/VkDeviceMemory 留 NULL_HANDLE.
    // Phase 1.3 換成 vkCreateBuffer + vkAllocateMemory + vkMapMemory.
    auto buf = std::make_shared<VkFrucCpuBitstreamBuffer>(
        (size_t)size,
        (size_t)minBitstreamBufferOffsetAlignment,
        (size_t)minBitstreamBufferSizeAlignment);
    if (pInitializeBufferMemory && initializeBufferMemorySize) {
        VkDeviceSize maxSz = 0;
        uint8_t* dst = buf->GetDataPtr(0, maxSz);
        if (dst && initializeBufferMemorySize <= maxSz) {
            memcpy(dst, pInitializeBufferMemory, (size_t)initializeBufferMemorySize);
        }
    }
    bitstreamBuffer = buf;
    return size;
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
// VkFrucRenderer parser instance management (defined here to avoid pulling
// nvvideoparser headers into vkfruc.cpp's already-massive #include set)
// =============================================================================

bool VkFrucRenderer::createNvVideoParser()
{
    auto pimpl = new NvParserPimpl();
    pimpl->client = std::make_shared<VkFrucDecodeClient>(this);

    VkExtensionProperties stdHeaderVer = {};
    VkVideoCodecOperationFlagBitsKHR codecOp;
    if (m_VideoCodec & VIDEO_FORMAT_MASK_H265) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else if (m_VideoCodec & VIDEO_FORMAT_MASK_H264) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    } else if (m_VideoCodec & VIDEO_FORMAT_MASK_AV1) {
        strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName),
                  VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME, _TRUNCATE);
        stdHeaderVer.specVersion = VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION;
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    } else {
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
