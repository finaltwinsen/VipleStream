#if defined(__x86_64__) || defined(_M_X64)
// VipleStream §M2a-R4：gcc 不再對整個 nvvideoparser 加全域 -mssse3 等旗標（見 nvvideoparser.pro），
// 只有這個 TU 開 SSSE3（_mm_alignr_epi8 需要；cpudetect.cpp 選 SSSE3 路徑時檢查的就是 ssse3）。
// pragma 必須在任何 #include 之前，標頭裡被這裡實體化的 template／inline 函式才會用同一個 target。
// MSVC 不需要，clang 仍走 .pro 的全域旗標。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC target("ssse3")
#endif
#include <immintrin.h>
#include <cpudetect.h>
#include "ByteStreamParser.h"
#include "NvVideoParser/nvVulkanVideoUtils.h"
#include "NvVideoParser/nvVulkanVideoParser.h"

bool VulkanVideoDecoder::ParseByteStreamSSSE3(const VkParserBitstreamPacket* pck, size_t *pParsedBytes)
{
    return ParseByteStreamSimd<SIMD_ISA::SSSE3>(pck, pParsedBytes);
}

template<>
size_t VulkanVideoDecoder::next_start_code<SIMD_ISA::SSSE3>(const uint8_t *pdatain, size_t datasize, bool& found_start_code)
{
    size_t i = 0;
    size_t datasize32 = (datasize >> 5) << 5;
    if (datasize32 > 32)
    {
        const __m128i v1 = _mm_set1_epi8(1);
        __m128i vdata = _mm_loadu_si128((const __m128i*)pdatain);
        __m128i vBfr = _mm_set1_epi16(((m_BitBfr << 8) & 0xFF00) | ((m_BitBfr >> 8) & 0xFF));
        __m128i vdata_prev1 = _mm_alignr_epi8(vdata, vBfr, 15);
        __m128i vdata_prev2 = _mm_alignr_epi8(vdata, vBfr, 14);
        for ( ; i < datasize32 - 32; i += 32)
        {
            for (int c = 0; c < 32; c += 16)
            {
                // hotspot begin
                __m128i vdata_prev1or2 = _mm_or_si128(vdata_prev2, vdata_prev1);
                __m128i vmask = _mm_cmpeq_epi8(_mm_and_si128(vdata, _mm_cmpeq_epi8(vdata_prev1or2, _mm_setzero_si128())), v1);
                const int resmask = _mm_movemask_epi8(vmask);
                // hotspot end
                if (resmask)
                {
                    const int offset = count_trailing_zeros((uint64_t) (resmask & 0xFFFFFFFF));
                    found_start_code = true;
                    m_BitBfr =  1;
                    return offset + i + c + 1;
                }
                // hotspot begin
                __m128i vdata_next = _mm_loadu_si128((const __m128i*)&pdatain[i + c + 16]);
                vdata_prev1 = _mm_alignr_epi8(vdata_next, vdata, 15);
                vdata_prev2 = _mm_alignr_epi8(vdata_next, vdata, 14);
                vdata = vdata_next;
                // hotspot end
            }
        } // main processing loop end
        m_BitBfr = (pdatain[i-2] << 8) | pdatain[i-1];
    }
    // process a tail (rest):
    uint32_t bfr = m_BitBfr;
    do
    {
        bfr = (bfr << 8) | pdatain[i++];
        if ((bfr & 0x00ffffff) == 1) {
            break;
        }
    } while (i < datasize);
    m_BitBfr = bfr;
    found_start_code = ((bfr & 0x00ffffff) == 1);
    return i;
}
#endif