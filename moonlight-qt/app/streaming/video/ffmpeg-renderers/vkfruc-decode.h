// VipleStream §J.3.e.2.i.8 — VkFruc native VK_KHR_video_decode glue.
//
// Implements VkParserVideoDecodeClient (lower-level NvVideoParser callback
// interface) instead of high-level IVulkanVideoDecoderHandler — saves
// pulling VulkanVideoParser.cpp (~2700 LOC) bridge + ycbcr_utils deps.
//
// Wire 流程:
//   submitNativeDecodeUnit(NAL bytes)
//     → VkFrucNvParserGlue::ParseByteStream
//     → parser internal:
//         BeginSequence (first SPS)        → log + (Phase 1.3) reconfigure session
//         UpdatePictureParameters (VPS/SPS/PPS)  → vkUpdateVideoSessionParametersKHR
//         AllocPictureBuffer (DPB slot)    → (Phase 1.3) allocate VkImage
//         DecodePicture (slice data)       → (Phase 1.3) vkCmdDecodeVideoKHR
//         DisplayPicture (decoded ready)   → (Phase 1.4) feed to graphics pipeline

#pragma once

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "vkvideo_parser/VulkanVideoParserIf.h"
#include "vkvideo_parser/VulkanVideoParserParams.h"
#include "vkvideo_parser/StdVideoPictureParametersSet.h"
#include "NvVideoParser/nvVulkanVideoParser.h"
#include <atomic>
#include <memory>

class VkFrucRenderer;

class VkFrucDecodeClient : public VkParserVideoDecodeClient
{
public:
    explicit VkFrucDecodeClient(VkFrucRenderer* parent) : m_Parent(parent) {}
    ~VkFrucDecodeClient() override = default;

    // Returns max number of reference frames. 用 H.265 max DPB slots
    int32_t BeginSequence(const VkParserSequenceInfo* pnvsi) override;

    // Allocate VkPicIf for parser to track DPB picture slot.  Phase 1.3.
    bool    AllocPictureBuffer(VkPicIf** ppPicBuf) override;

    // VPS/SPS/PPS NAL parsed.  Forward to vkUpdateVideoSessionParametersKHR.
    bool    UpdatePictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersObject,
                                    VkSharedBaseObj<VkVideoRefCountBase>& client) override;

    // Slice parsed, ready to decode.  Phase 1.3 vkCmdDecodeVideoKHR.
    bool    DecodePicture(VkParserPictureData* pParserPictureData) override;

    // Decoded picture ready.  Phase 1.4 hand to graphics pipeline.
    bool    DisplayPicture(VkPicIf* pPicBuf, int64_t llPTS) override;

    void    UnhandledNALU(const uint8_t* pbData, size_t cbData) override;

    // 提供 GPU bitstream buffer for parser to upload NAL bytes.  Phase 1.3.
    VkDeviceSize GetBitstreamBuffer(VkDeviceSize size,
                                    VkDeviceSize minBitstreamBufferOffsetAlignment,
                                    VkDeviceSize minBitstreamBufferSizeAlignment,
                                    const uint8_t* pInitializeBufferMemory,
                                    VkDeviceSize initializeBufferMemorySize,
                                    VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer) override;

    // Diagnostic counters
    uint64_t getBeginSeqCount()       const { return m_BeginSeqCount.load(); }
    uint64_t getUpdateParamsCount()   const { return m_UpdateParamsCount.load(); }
    uint64_t getDecodePicCount()      const { return m_DecodePicCount.load(); }
    uint64_t getDisplayPicCount()     const { return m_DisplayPicCount.load(); }

    // §J.3.e.2.i.8 Phase 1.3b — DPB picture buffer pool with VkImage backing.
    // 17 slots = H.265 max DPB.  Each slot owns its own VkImage / VkDeviceMemory /
    // VkImageView (allocated lazily on first BeginSequence by the renderer's
    // ensureDpbImagePool helper).  vkPicBuffBase tracks parser refcount + slot id.
    struct VkFrucDpbPicture : public vkPicBuffBase {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
    };
    static constexpr int kPicPoolSize = 17;

    // Public so VkFrucRenderer can iterate slots to populate VkImages from
    // ensureDpbImagePool / tear them down on destroyVideoSession.
    VkFrucDpbPicture       m_PicPool[kPicPoolSize];

private:
    VkFrucRenderer*        m_Parent;
    std::atomic<uint64_t>  m_BeginSeqCount{0};
    std::atomic<uint64_t>  m_UpdateParamsCount{0};
    std::atomic<uint64_t>  m_DecodePicCount{0};
    std::atomic<uint64_t>  m_DisplayPicCount{0};
};

#endif // HAVE_LIBPLACEBO_VULKAN
