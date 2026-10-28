#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "Limelight.h"
}

#define PW_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PyroWaveBridge", __VA_ARGS__)
#define PW_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "PyroWaveBridge", __VA_ARGS__)
#define PW_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PyroWaveBridge", __VA_ARGS__)

static thread_local PDECODE_UNIT g_tls_decode_unit = nullptr;

extern "C" void PyroWaveAndroid_SetCurrentDecodeUnit(PDECODE_UNIT du)
{
    g_tls_decode_unit = du;
}

#if defined(VIPLESTREAM_HAVE_PYROWAVE)

#include "vulkan_headers.hpp"
#include "wsi.hpp"
#include "context.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include "image.hpp"
#include "shader.hpp"
#include "fence.hpp"
#include "thread_id.hpp"
#include "pyrowave_decoder.hpp"
#include "pyrowave_common.hpp"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <mutex>
#include <memory>
#include <vector>

namespace
{

// Fullscreen triangle vertex shader (SPIR-V 1.0):
//   layout(location = 0) out vec2 vUV;
//   void main() {
//       vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
//       gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
//   }
static const uint32_t kPyroWavePresentVertSpv[] = {
    0x07230203u, 0x00010000u, 0x000d000bu, 0x0000002cu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0008000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000cu, 0x0000001du,
    0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000cu, 0x0000000bu, 0x0000002au,
    0x00030047u, 0x0000001bu, 0x00000002u, 0x00050048u, 0x0000001bu, 0x00000000u, 0x0000000bu, 0x00000000u,
    0x00050048u, 0x0000001bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000001bu, 0x00000002u,
    0x0000000bu, 0x00000003u, 0x00050048u, 0x0000001bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00020013u,
    0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
    0x00000007u, 0x00000006u, 0x00000002u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu,
    0x00000008u, 0x00000009u, 0x00000003u, 0x00040015u, 0x0000000au, 0x00000020u, 0x00000001u, 0x00040020u,
    0x0000000bu, 0x00000001u, 0x0000000au, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000001u, 0x0004002bu,
    0x0000000au, 0x0000000eu, 0x00000001u, 0x0004002bu, 0x0000000au, 0x00000010u, 0x00000002u, 0x00040017u,
    0x00000017u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000018u, 0x00000020u, 0x00000000u, 0x0004002bu,
    0x00000018u, 0x00000019u, 0x00000001u, 0x0004001cu, 0x0000001au, 0x00000006u, 0x00000019u, 0x0006001eu,
    0x0000001bu, 0x00000017u, 0x00000006u, 0x0000001au, 0x0000001au, 0x00040020u, 0x0000001cu, 0x00000003u,
    0x0000001bu, 0x0004003bu, 0x0000001cu, 0x0000001du, 0x00000003u, 0x0004002bu, 0x0000000au, 0x0000001eu,
    0x00000000u, 0x0004002bu, 0x00000006u, 0x00000020u, 0x40000000u, 0x0004002bu, 0x00000006u, 0x00000022u,
    0x3f800000u, 0x0004002bu, 0x00000006u, 0x00000025u, 0x00000000u, 0x00040020u, 0x00000029u, 0x00000003u,
    0x00000017u, 0x0005002cu, 0x00000007u, 0x0000002bu, 0x00000022u, 0x00000022u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000au, 0x0000000du,
    0x0000000cu, 0x000500c4u, 0x0000000au, 0x0000000fu, 0x0000000du, 0x0000000eu, 0x000500c7u, 0x0000000au,
    0x00000011u, 0x0000000fu, 0x00000010u, 0x0004006fu, 0x00000006u, 0x00000012u, 0x00000011u, 0x000500c7u,
    0x0000000au, 0x00000014u, 0x0000000du, 0x00000010u, 0x0004006fu, 0x00000006u, 0x00000015u, 0x00000014u,
    0x00050050u, 0x00000007u, 0x00000016u, 0x00000012u, 0x00000015u, 0x0003003eu, 0x00000009u, 0x00000016u,
    0x0004003du, 0x00000007u, 0x0000001fu, 0x00000009u, 0x0005008eu, 0x00000007u, 0x00000021u, 0x0000001fu,
    0x00000020u, 0x00050083u, 0x00000007u, 0x00000024u, 0x00000021u, 0x0000002bu, 0x00050051u, 0x00000006u,
    0x00000026u, 0x00000024u, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000027u, 0x00000024u, 0x00000001u,
    0x00070050u, 0x00000017u, 0x00000028u, 0x00000026u, 0x00000027u, 0x00000025u, 0x00000022u, 0x00050041u,
    0x00000029u, 0x0000002au, 0x0000001du, 0x0000001eu, 0x0003003eu, 0x0000002au, 0x00000028u, 0x000100fdu,
    0x00010038u
};

// YUV (Y, U, V planes at set 0 bindings 0, 1, 2) -> RGB fragment shader (SPIR-V 1.0):
//   layout(push_constant) uniform Push { uint color_space; uint full_range; } push_data;
static const uint32_t kPyroWavePresentFragSpv[] = {
    0x07230203u, 0x00010000u, 0x000d000bu, 0x000000c0u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000010u, 0x00000097u, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00040047u, 0x0000000cu, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000000cu,
    0x00000022u, 0x00000000u, 0x00040047u, 0x00000010u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000018u,
    0x00000021u, 0x00000001u, 0x00040047u, 0x00000018u, 0x00000022u, 0x00000000u, 0x00040047u, 0x0000001eu,
    0x00000021u, 0x00000002u, 0x00040047u, 0x0000001eu, 0x00000022u, 0x00000000u, 0x00030047u, 0x00000023u,
    0x00000002u, 0x00050048u, 0x00000023u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00050048u, 0x00000023u,
    0x00000001u, 0x00000023u, 0x00000004u, 0x00040047u, 0x00000097u, 0x0000001eu, 0x00000000u, 0x00020013u,
    0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00090019u,
    0x00000009u, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u,
    0x0003001bu, 0x0000000au, 0x00000009u, 0x00040020u, 0x0000000bu, 0x00000000u, 0x0000000au, 0x0004003bu,
    0x0000000bu, 0x0000000cu, 0x00000000u, 0x00040017u, 0x0000000eu, 0x00000006u, 0x00000002u, 0x00040020u,
    0x0000000fu, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000000fu, 0x00000010u, 0x00000001u, 0x00040017u,
    0x00000012u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000014u, 0x00000020u, 0x00000000u, 0x0004002bu,
    0x00000014u, 0x00000015u, 0x00000000u, 0x0004003bu, 0x0000000bu, 0x00000018u, 0x00000000u, 0x0004003bu,
    0x0000000bu, 0x0000001eu, 0x00000000u, 0x0004001eu, 0x00000023u, 0x00000014u, 0x00000014u, 0x00040020u,
    0x00000024u, 0x00000009u, 0x00000023u, 0x0004003bu, 0x00000024u, 0x00000025u, 0x00000009u, 0x00040015u,
    0x00000026u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000026u, 0x00000027u, 0x00000001u, 0x00040020u,
    0x00000028u, 0x00000009u, 0x00000014u, 0x00020014u, 0x0000002bu, 0x0004002bu, 0x00000006u, 0x00000030u,
    0x3d808081u, 0x0004002bu, 0x00000006u, 0x00000032u, 0x3f950a85u, 0x0004002bu, 0x00000006u, 0x00000035u,
    0x3f008081u, 0x0004002bu, 0x00000006u, 0x00000037u, 0x3f91b6dbu, 0x0004002bu, 0x00000006u, 0x0000003du,
    0x3f000000u, 0x0004002bu, 0x00000026u, 0x00000042u, 0x00000000u, 0x0004002bu, 0x00000014u, 0x00000045u,
    0x00000002u, 0x00040017u, 0x00000049u, 0x00000006u, 0x00000003u, 0x0004002bu, 0x00000006u, 0x0000004du,
    0x3fbcbfb1u, 0x0004002bu, 0x00000006u, 0x00000053u, 0x3e288095u, 0x0004002bu, 0x00000006u, 0x00000057u,
    0x3f124431u, 0x0004002bu, 0x00000006u, 0x0000005eu, 0x3ff0d1b7u, 0x0004002bu, 0x00000006u, 0x0000006au,
    0x3fb374bcu, 0x0004002bu, 0x00000006u, 0x00000070u, 0x3eb03298u, 0x0004002bu, 0x00000006u, 0x00000074u,
    0x3f36d19eu, 0x0004002bu, 0x00000006u, 0x0000007au, 0x3fe2d0e5u, 0x0004002bu, 0x00000006u, 0x00000081u,
    0x3fc9930cu, 0x0004002bu, 0x00000006u, 0x00000087u, 0x3e3fd1ddu, 0x0004002bu, 0x00000006u, 0x0000008bu,
    0x3eefadf3u, 0x0004002bu, 0x00000006u, 0x00000091u, 0x3fed844du, 0x00040020u, 0x00000096u, 0x00000003u,
    0x00000012u, 0x0004003bu, 0x00000096u, 0x00000097u, 0x00000003u, 0x0004002bu, 0x00000006u, 0x00000099u,
    0x00000000u, 0x0004002bu, 0x00000006u, 0x0000009au, 0x3f800000u, 0x0006002cu, 0x00000049u, 0x000000bbu,
    0x00000099u, 0x00000099u, 0x00000099u, 0x0006002cu, 0x00000049u, 0x000000bcu, 0x0000009au, 0x0000009au,
    0x0000009au, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x0004003du, 0x0000000au, 0x0000000du, 0x0000000cu, 0x0004003du, 0x0000000eu, 0x00000011u, 0x00000010u,
    0x00050057u, 0x00000012u, 0x00000013u, 0x0000000du, 0x00000011u, 0x00050051u, 0x00000006u, 0x00000016u,
    0x00000013u, 0x00000000u, 0x0004003du, 0x0000000au, 0x00000019u, 0x00000018u, 0x00050057u, 0x00000012u,
    0x0000001bu, 0x00000019u, 0x00000011u, 0x00050051u, 0x00000006u, 0x0000001cu, 0x0000001bu, 0x00000000u,
    0x0004003du, 0x0000000au, 0x0000001fu, 0x0000001eu, 0x00050057u, 0x00000012u, 0x00000021u, 0x0000001fu,
    0x00000011u, 0x00050051u, 0x00000006u, 0x00000022u, 0x00000021u, 0x00000000u, 0x00050041u, 0x00000028u,
    0x00000029u, 0x00000025u, 0x00000027u, 0x0004003du, 0x00000014u, 0x0000002au, 0x00000029u, 0x000500aau,
    0x0000002bu, 0x0000002cu, 0x0000002au, 0x00000015u, 0x000300f7u, 0x0000002eu, 0x00000000u, 0x000400fau,
    0x0000002cu, 0x0000002du, 0x0000003cu, 0x000200f8u, 0x0000002du, 0x00050083u, 0x00000006u, 0x00000031u,
    0x00000016u, 0x00000030u, 0x00050085u, 0x00000006u, 0x00000033u, 0x00000031u, 0x00000032u, 0x00050083u,
    0x00000006u, 0x00000036u, 0x0000001cu, 0x00000035u, 0x00050085u, 0x00000006u, 0x00000038u, 0x00000036u,
    0x00000037u, 0x00050083u, 0x00000006u, 0x0000003au, 0x00000022u, 0x00000035u, 0x00050085u, 0x00000006u,
    0x0000003bu, 0x0000003au, 0x00000037u, 0x000200f9u, 0x0000002eu, 0x000200f8u, 0x0000003cu, 0x00050083u,
    0x00000006u, 0x0000003fu, 0x0000001cu, 0x0000003du, 0x00050083u, 0x00000006u, 0x00000041u, 0x00000022u,
    0x0000003du, 0x000200f9u, 0x0000002eu, 0x000200f8u, 0x0000002eu, 0x000700f5u, 0x00000006u, 0x000000b8u,
    0x00000038u, 0x0000002du, 0x0000003fu, 0x0000003cu, 0x000700f5u, 0x00000006u, 0x000000b5u, 0x0000003bu,
    0x0000002du, 0x00000041u, 0x0000003cu, 0x000700f5u, 0x00000006u, 0x000000b4u, 0x00000033u, 0x0000002du,
    0x00000016u, 0x0000003cu, 0x00050041u, 0x00000028u, 0x00000043u, 0x00000025u, 0x00000042u, 0x0004003du,
    0x00000014u, 0x00000044u, 0x00000043u, 0x000500aau, 0x0000002bu, 0x00000046u, 0x00000044u, 0x00000045u,
    0x000300f7u, 0x00000048u, 0x00000000u, 0x000400fau, 0x00000046u, 0x00000047u, 0x00000063u, 0x000200f8u,
    0x00000047u, 0x00050085u, 0x00000006u, 0x0000004fu, 0x0000004du, 0x000000b5u, 0x00050081u, 0x00000006u,
    0x00000050u, 0x000000b4u, 0x0000004fu, 0x00050085u, 0x00000006u, 0x00000055u, 0x00000053u, 0x000000b8u,
    0x00050083u, 0x00000006u, 0x00000056u, 0x000000b4u, 0x00000055u, 0x00050085u, 0x00000006u, 0x00000059u,
    0x00000057u, 0x000000b5u, 0x00050083u, 0x00000006u, 0x0000005au, 0x00000056u, 0x00000059u, 0x00050085u,
    0x00000006u, 0x00000060u, 0x0000005eu, 0x000000b8u, 0x00050081u, 0x00000006u, 0x00000061u, 0x000000b4u,
    0x00000060u, 0x00060050u, 0x00000049u, 0x000000bfu, 0x00000050u, 0x0000005au, 0x00000061u, 0x000200f9u,
    0x00000048u, 0x000200f8u, 0x00000063u, 0x000500aau, 0x0000002bu, 0x00000066u, 0x00000044u, 0x00000015u,
    0x000300f7u, 0x00000068u, 0x00000000u, 0x000400fau, 0x00000066u, 0x00000067u, 0x0000007fu, 0x000200f8u,
    0x00000067u, 0x00050085u, 0x00000006u, 0x0000006cu, 0x0000006au, 0x000000b5u, 0x00050081u, 0x00000006u,
    0x0000006du, 0x000000b4u, 0x0000006cu, 0x00050085u, 0x00000006u, 0x00000072u, 0x00000070u, 0x000000b8u,
    0x00050083u, 0x00000006u, 0x00000073u, 0x000000b4u, 0x00000072u, 0x00050085u, 0x00000006u, 0x00000076u,
    0x00000074u, 0x000000b5u, 0x00050083u, 0x00000006u, 0x00000077u, 0x00000073u, 0x00000076u, 0x00050085u,
    0x00000006u, 0x0000007cu, 0x0000007au, 0x000000b8u, 0x00050081u, 0x00000006u, 0x0000007du, 0x000000b4u,
    0x0000007cu, 0x00060050u, 0x00000049u, 0x000000beu, 0x0000006du, 0x00000077u, 0x0000007du, 0x000200f9u,
    0x00000068u, 0x000200f8u, 0x0000007fu, 0x00050085u, 0x00000006u, 0x00000083u, 0x00000081u, 0x000000b5u,
    0x00050081u, 0x00000006u, 0x00000084u, 0x000000b4u, 0x00000083u, 0x00050085u, 0x00000006u, 0x00000089u,
    0x00000087u, 0x000000b8u, 0x00050083u, 0x00000006u, 0x0000008au, 0x000000b4u, 0x00000089u, 0x00050085u,
    0x00000006u, 0x0000008du, 0x0000008bu, 0x000000b5u, 0x00050083u, 0x00000006u, 0x0000008eu, 0x0000008au,
    0x0000008du, 0x00050085u, 0x00000006u, 0x00000093u, 0x00000091u, 0x000000b8u, 0x00050081u, 0x00000006u,
    0x00000094u, 0x000000b4u, 0x00000093u, 0x00060050u, 0x00000049u, 0x000000bdu, 0x00000084u, 0x0000008eu,
    0x00000094u, 0x000200f9u, 0x00000068u, 0x000200f8u, 0x00000068u, 0x000700f5u, 0x00000049u, 0x000000bau,
    0x000000beu, 0x00000067u, 0x000000bdu, 0x0000007fu, 0x000200f9u, 0x00000048u, 0x000200f8u, 0x00000048u,
    0x000700f5u, 0x00000049u, 0x000000b9u, 0x000000bfu, 0x00000047u, 0x000000bau, 0x00000068u, 0x0008000cu,
    0x00000049u, 0x0000009du, 0x00000001u, 0x0000002bu, 0x000000b9u, 0x000000bbu, 0x000000bcu, 0x00050051u,
    0x00000006u, 0x0000009eu, 0x0000009du, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000009fu, 0x0000009du,
    0x00000001u, 0x00050051u, 0x00000006u, 0x000000a0u, 0x0000009du, 0x00000002u, 0x00070050u, 0x00000012u,
    0x000000a1u, 0x0000009eu, 0x0000009fu, 0x000000a0u, 0x0000009au, 0x0003003eu, 0x00000097u, 0x000000a1u,
    0x000100fdu, 0x00010038u
};

struct PresentPushConstants
{
    uint32_t color_space;
    uint32_t full_range;
};

class AndroidWSIPlatform final : public Vulkan::WSIPlatform
{
public:
    AndroidWSIPlatform(ANativeWindow *win, uint32_t fallback_w, uint32_t fallback_h)
        : window(win), fallback_width(fallback_w), fallback_height(fallback_h)
    {
    }

    ~AndroidWSIPlatform() override
    {
        if (window)
        {
            ANativeWindow_release(window);
            window = nullptr;
        }
    }

    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
    {
        if (!window)
            return VK_NULL_HANDLE;

        VkAndroidSurfaceCreateInfoKHR create_info = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        create_info.window = window;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateAndroidSurfaceKHR(instance, &create_info, nullptr, &surface) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return surface;
    }

    std::vector<const char *> get_instance_extensions() override
    {
        return { "VK_KHR_surface", "VK_KHR_android_surface" };
    }

    uint32_t get_surface_width() override
    {
        if (window)
        {
            int32_t w = ANativeWindow_getWidth(window);
            if (w > 0)
                return static_cast<uint32_t>(w);
        }
        return fallback_width;
    }

    uint32_t get_surface_height() override
    {
        if (window)
        {
            int32_t h = ANativeWindow_getHeight(window);
            if (h > 0)
                return static_cast<uint32_t>(h);
        }
        return fallback_height;
    }

    bool alive(Vulkan::WSI &) override
    {
        return window != nullptr;
    }

    void poll_input() override {}
    void poll_input_async(Granite::InputTrackerHandler *) override {}

    void check_dimension_change()
    {
        uint32_t w = get_surface_width();
        uint32_t h = get_surface_height();
        if (w > 0 && h > 0 && (w != current_swapchain_width || h != current_swapchain_height))
            resize = true;
    }

private:
    ANativeWindow *window = nullptr;
    uint32_t fallback_width = 0;
    uint32_t fallback_height = 0;
};

struct PyroWaveAndroidSession
{
    std::unique_ptr<AndroidWSIPlatform> platform;
    std::unique_ptr<Vulkan::WSI> wsi;
    PyroWave::Decoder decoder;
    Vulkan::ImageHandle planes[3];
    // plane 與 decoder 內部的 wavelet 影像每幀重用，device 只有 2 個 frame context
    // （begin_frame 只保證 N-2 幀完成），所以下一幀 decode 前要等上一幀的 fence。
    Vulkan::Fence last_frame_fence;
    // plane 目前的 layout：建立時 UNDEFINED，每幀結束時轉成 READ_ONLY_OPTIMAL 給 present 取樣。
    VkImageLayout plane_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<uint8_t> frame_buffer;
    Vulkan::Program *present_program = nullptr;
    int width = 0;
    int height = 0;
    PyroWave::ChromaSubsampling chroma = PyroWave::ChromaSubsampling::Chroma420;
    bool is_10bit = false;
    bool fragment_path = false;
    uint32_t color_space = 1;
    uint32_t full_range = 0;
    bool initialized = false;
};

static std::mutex g_session_mutex;
static std::unique_ptr<PyroWaveAndroidSession> g_session;

static void cleanup_session_locked()
{
    if (!g_session)
        return;

    if (g_session->wsi && g_session->initialized)
    {
        Util::register_thread_index(0);
        g_session->last_frame_fence.reset();
        g_session->wsi->get_device().wait_idle();
    }
    for (int i = 0; i < 3; ++i)
    {
        g_session->planes[i].reset();
    }
    g_session.reset();
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_isPyroWaveSupported(JNIEnv *, jclass)
{
    static int s_supported = -1;
    if (s_supported >= 0)
        return s_supported ? JNI_TRUE : JNI_FALSE;

    if (!Vulkan::Context::init_loader(nullptr))
    {
        PW_LOGW("Vulkan loader initialization failed; PyroWave unavailable");
        s_supported = 0;
        return JNI_FALSE;
    }

    uint32_t version = volkGetInstanceVersion();
    if (version < VK_API_VERSION_1_1)
    {
        PW_LOGW("Vulkan instance version 0x%x < 1.1; PyroWave unavailable", version);
        s_supported = 0;
        return JNI_FALSE;
    }

    s_supported = 1;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveInit(
    JNIEnv *env, jclass, jobject jSurface,
    jint videoFormat, jint width, jint height,
    jint colorSpace, jint colorRange)
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    cleanup_session_locked();

    if (!jSurface || width <= 0 || height <= 0)
    {
        PW_LOGE("Invalid surface or dimensions (%dx%d)", width, height);
        return -1;
    }

    if (!Vulkan::Context::init_loader(nullptr))
    {
        PW_LOGE("Vulkan::Context::init_loader failed");
        return -2;
    }

    ANativeWindow *win = ANativeWindow_fromSurface(env, jSurface);
    if (!win)
    {
        PW_LOGE("ANativeWindow_fromSurface returned null");
        return -3;
    }

    auto session = std::make_unique<PyroWaveAndroidSession>();
    session->width = width;
    session->height = height;
    session->chroma = (videoFormat & VIDEO_FORMAT_MASK_YUV444)
                          ? PyroWave::ChromaSubsampling::Chroma444
                          : PyroWave::ChromaSubsampling::Chroma420;
    session->is_10bit = (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    session->color_space = static_cast<uint32_t>(colorSpace);
    session->full_range = (colorRange == COLOR_RANGE_FULL) ? 1u : 0u;

    session->platform = std::make_unique<AndroidWSIPlatform>(
        win, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    session->wsi = std::make_unique<Vulkan::WSI>();
    session->wsi->set_platform(session->platform.get());
    session->wsi->set_present_mode(Vulkan::PresentMode::UnlockedMaybeTear);
    session->wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);

    Vulkan::Context::SystemHandles sys_handles = {};
    if (!session->wsi->init_simple(1, sys_handles))
    {
        PW_LOGE("Vulkan::WSI::init_simple failed for %dx%d", width, height);
        return -4;
    }

    Vulkan::Device &device = session->wsi->get_device();
    session->fragment_path = !session->is_10bit && PyroWave::Decoder::device_prefers_fragment_path(device);

    if (!session->decoder.init(&device, width, height, session->chroma, session->fragment_path))
    {
        PW_LOGE("PyroWave::Decoder::init failed for %dx%d", width, height);
        return -5;
    }

    VkFormat plane_format = session->is_10bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    VkImageUsageFlags plane_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        (session->fragment_path ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT);

    for (int p = 0; p < 3; ++p)
    {
        uint32_t pw = static_cast<uint32_t>(width);
        uint32_t ph = static_cast<uint32_t>(height);
        if (p > 0 && session->chroma == PyroWave::ChromaSubsampling::Chroma420)
        {
            pw = (pw + 1u) >> 1u;
            ph = (ph + 1u) >> 1u;
        }
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(pw, ph, plane_format, false);
        info.usage = plane_usage;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.misc = Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
        session->planes[p] = device.create_image(info);
        if (!session->planes[p])
        {
            PW_LOGE("Failed to allocate PyroWave YUV plane %d (%ux%u)", p, pw, ph);
            return -6;
        }
    }

    Vulkan::ResourceLayout vert_layout = {};
    vert_layout.input_mask = 0;
    vert_layout.output_mask = 1u << 0;
    vert_layout.push_constant_size = 0;

    Vulkan::ResourceLayout frag_layout = {};
    frag_layout.input_mask = 1u << 0;
    frag_layout.output_mask = 1u << 0;
    frag_layout.push_constant_size = sizeof(PresentPushConstants);
    frag_layout.sets[0].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 2);
    frag_layout.sets[0].fp_mask = (1u << 0) | (1u << 1) | (1u << 2);
    frag_layout.sets[0].meta[0].array_size = 1;
    frag_layout.sets[0].meta[1].array_size = 1;
    frag_layout.sets[0].meta[2].array_size = 1;

    session->present_program = device.request_program(
        kPyroWavePresentVertSpv, sizeof(kPyroWavePresentVertSpv),
        kPyroWavePresentFragSpv, sizeof(kPyroWavePresentFragSpv),
        &vert_layout, &frag_layout);
    if (!session->present_program)
    {
        PW_LOGE("Failed to create PyroWave present shader program");
        return -7;
    }

    session->initialized = true;
    PW_LOGI("Initialized PyroWave Vulkan GPU decoder + swapchain (%dx%d, %s, %d-bit, fragment_path=%d)",
            width, height,
            session->chroma == PyroWave::ChromaSubsampling::Chroma444 ? "4:4:4" : "4:2:0",
            session->is_10bit ? 10 : 8,
            session->fragment_path ? 1 : 0);

    g_session = std::move(session);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveDecodeAndRender(
    JNIEnv *env, jclass, jbyteArray jData, jint length, jint /*frameType*/)
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (!g_session || !g_session->initialized)
        return DR_NEED_IDR;

    // init 在連線執行緒、decode 在 decode queue 執行緒；Granite 只認 index 0 的執行緒
    // （參考實作 pyrowave_c.cpp 也是每次 decode 前註冊）。
    Util::register_thread_index(0);

    g_session->decoder.clear();

    // server 送連續位元流，單一 PyroWave 封包可能跨好幾個 RTP 片段：整幀接成一段再 push。
    // 不完整的幀接起來會錯位，先整幀丟掉（每幀都是 intra，下一幀就恢復）。
    const bool allow_partial = false;
    PDECODE_UNIT du = g_tls_decode_unit;
    if (du != nullptr)
    {
        if (du->flags & DU_FLAG_PARTIAL_FRAME)
            return DR_OK;

        auto &buf = g_session->frame_buffer;
        buf.clear();
        buf.reserve(static_cast<size_t>(du->fullLength));
        for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next)
        {
            if (entry->data && entry->length > 0)
            {
                auto *p = reinterpret_cast<const uint8_t *>(entry->data);
                buf.insert(buf.end(), p, p + entry->length);
            }
        }
        if (!g_session->decoder.push_packet(buf.data(), buf.size()))
        {
            PW_LOGW("push_packet failed on frame %d (len=%zu)", du->frameNumber, buf.size());
            return DR_OK;
        }
    }
    else if (jData != nullptr && length > 0)
    {
        jbyte *raw = env->GetByteArrayElements(jData, nullptr);
        if (!raw)
            return DR_OK;
        bool ok = g_session->decoder.push_packet(raw, static_cast<size_t>(length));
        env->ReleaseByteArrayElements(jData, raw, JNI_ABORT);
        if (!ok)
        {
            PW_LOGW("push_packet (fallback byte[]) failed (len=%d)", length);
            return DR_OK;
        }
    }
    else
    {
        return DR_OK;
    }

    if (!g_session->decoder.decode_is_ready(allow_partial))
    {
        return DR_OK;
    }

    g_session->platform->check_dimension_change();
    if (!g_session->wsi->begin_frame())
    {
        return DR_OK;
    }

    Vulkan::Device &device = g_session->wsi->get_device();

    if (g_session->last_frame_fence)
    {
        g_session->last_frame_fence->wait();
        g_session->last_frame_fence.reset();
    }

    auto cmd = device.request_command_buffer();

    // decoder 不會自己轉 plane 的 layout：compute path 以 storage image（GENERAL）寫入，
    // fragment path 以 color attachment（ATTACHMENT_OPTIMAL）寫入（同 pyrowave_c.cpp）。
    VkImageLayout decode_layout = g_session->fragment_path
        ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags2 dst_stage = g_session->fragment_path
        ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
        : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkAccessFlags2 dst_access = g_session->fragment_path
        ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
        : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    for (int p = 0; p < 3; ++p)
    {
        cmd->image_barrier(*g_session->planes[p],
                           g_session->plane_layout, decode_layout,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                           dst_stage, dst_access);
    }

    PyroWave::ViewBuffers views = {};
    for (int p = 0; p < 3; ++p)
    {
        views.planes[p] = &g_session->planes[p]->get_view();
    }
    // 10-bit 輸出到 R16_UNORM：iDWT 最後一級要把 0..1023 縮到 UNORM16（參考實作同值），
    // 不設會幾乎全黑。fragment path 不支援 range_scale，init 時 10-bit 已強制走 compute。
    views.range_scale = g_session->is_10bit ? (float(0x3ff) / float(0xffff)) : 1.0f;

    if (!g_session->decoder.decode(*cmd, views))
    {
        PW_LOGW("PyroWave::Decoder::decode failed");
        // 已錄的 barrier 照樣送出，plane_layout 才會跟實際一致
        device.submit(cmd);
        g_session->plane_layout = decode_layout;
        g_session->wsi->end_frame();
        return DR_OK;
    }

    VkPipelineStageFlags2 src_stage = dst_stage;
    VkAccessFlags2 src_access = dst_access;

    for (int p = 0; p < 3; ++p)
    {
        cmd->image_barrier(*g_session->planes[p],
                           decode_layout,
                           VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                           src_stage, src_access,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    auto rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
    cmd->begin_render_pass(rp);
    cmd->set_program(g_session->present_program);
    cmd->set_quad_state();
    cmd->set_texture(0, 0, g_session->planes[0]->get_view(), Vulkan::StockSampler::LinearClamp);
    cmd->set_texture(0, 1, g_session->planes[1]->get_view(), Vulkan::StockSampler::LinearClamp);
    cmd->set_texture(0, 2, g_session->planes[2]->get_view(), Vulkan::StockSampler::LinearClamp);

    PresentPushConstants push = { g_session->color_space, g_session->full_range };
    cmd->push_constants(&push, 0, sizeof(push));
    cmd->draw(3);
    cmd->end_render_pass();

    device.submit(cmd, &g_session->last_frame_fence);
    g_session->plane_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    g_session->wsi->end_frame();

    return DR_OK;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveCleanup(JNIEnv *, jclass)
{
    std::lock_guard<std::mutex> lock(g_session_mutex);
    cleanup_session_locked();
}

#else // !VIPLESTREAM_HAVE_PYROWAVE

extern "C" JNIEXPORT jboolean JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_isPyroWaveSupported(JNIEnv *, jclass)
{
    return JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveInit(
    JNIEnv *, jclass, jobject, jint, jint, jint, jint, jint)
{
    return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveDecodeAndRender(
    JNIEnv *, jclass, jbyteArray, jint, jint)
{
    return DR_NEED_IDR;
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_nvstream_jni_MoonBridge_pyroWaveCleanup(JNIEnv *, jclass)
{
}

#endif // VIPLESTREAM_HAVE_PYROWAVE
