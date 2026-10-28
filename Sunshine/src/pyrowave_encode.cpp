/**
 * @file src/pyrowave_encode.cpp
 * @brief PyroWave GPU wavelet video encoder integration for Sunshine.
 */
#include "pyrowave_encode.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#  include <winsock2.h>
#  include <Windows.h>
#  include <winternl.h>
#  include <d3d11.h>
#  include <d3d11_4.h>
#  include <dxgi1_2.h>
#  include "platform/windows/display_vram.h"
#endif

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <moonlight-common-c/src/Video.h>
}

#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include "config.h"
#include "logging.h"
#include "utility.h"

using namespace std::literals;

namespace pyrowave {

  bool is_available() {
    static std::once_flag s_once;
    static bool s_available = false;

    std::call_once(s_once, []() {
      pyrowave_device dev = nullptr;
      pyrowave_result res = pyrowave_create_default_device(&dev);
      if (res == PYROWAVE_SUCCESS && dev != nullptr) {
        pyrowave_device_destroy(dev);
        s_available = true;
        BOOST_LOG(info) << "[VIPLE-PYROWAVE] Vulkan wavelet encoder probe: AVAILABLE";
      } else {
        s_available = false;
        BOOST_LOG(warning) << "[VIPLE-PYROWAVE] Vulkan wavelet encoder probe: UNAVAILABLE (err=" << (int) res << ")";
      }
    });

    return s_available;
  }

  struct pyrowave_encode_session_t::impl_t {
    video::config_t config {};
    video::sunshine_colorspace_t colorspace {};
    int capture_width = 0;
    int capture_height = 0;

    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;
    size_t max_bitstream_size = 0;
    bool has_encoded_frame = false;

#ifdef _WIN32
    struct imported_d3d_img_t {
      ID3D11Texture2D *capture_texture_p = nullptr;
      HANDLE orig_tex_handle = nullptr;
      HANDLE orig_fence_handle = nullptr;
      pyrowave_image image = nullptr;
      pyrowave_sync_object sync = nullptr;
      VkFormat vk_format = VK_FORMAT_UNDEFINED;
      uint32_t width = 0;
      uint32_t height = 0;
      std::weak_ptr<const platf::img_t> img_weak;

      void reset() {
        if (image) {
          pyrowave_image_destroy(image);
          image = nullptr;
        }
        if (sync) {
          pyrowave_sync_object_destroy(sync);
          sync = nullptr;
        }
        capture_texture_p = nullptr;
        orig_tex_handle = nullptr;
        orig_fence_handle = nullptr;
        vk_format = VK_FORMAT_UNDEFINED;
        width = 0;
        height = 0;
        img_weak.reset();
      }

      ~imported_d3d_img_t() {
        reset();
      }
    };

    std::map<uint32_t, imported_d3d_img_t> d3d_cache;
#endif

    // CPU fallback scaler & planar buffers
    video::sws_t sws;
    video::avcodec_frame_t sws_in_frame;
    video::avcodec_frame_t sw_frame;
    int sws_src_w = 0;
    int sws_src_h = 0;

    std::vector<uint8_t> raw_bitstream_buf;
    std::vector<pyrowave_packet> packet_descs;

    ~impl_t() {
#ifdef _WIN32
      d3d_cache.clear();
#endif
      if (encoder) {
        pyrowave_encoder_destroy(encoder);
        encoder = nullptr;
      }
      if (device) {
        pyrowave_device_destroy(device);
        device = nullptr;
      }
    }

    void update_rate_control() {
      size_t fps = (size_t) std::max(1, config.framerate);
      size_t bytes_per_frame = ((size_t) std::max(100, config.bitrate) * 1000ULL) / (8ULL * fps);
      max_bitstream_size = std::max<size_t>(4096, bytes_per_frame & ~size_t(3));
      if (raw_bitstream_buf.size() < max_bitstream_size + 65536) {
        raw_bitstream_buf.resize(max_bitstream_size + 65536);
      }
    }

    bool ensure_device_and_encoder(const platf::img_t *first_img) {
      if (encoder && device) {
        return true;
      }

      pyrowave_result res = PYROWAVE_ERROR_GENERIC;

#ifdef _WIN32
      if (first_img) {
        if (auto d3d_img = dynamic_cast<const platf::dxgi::img_d3d_t *>(first_img)) {
          if (d3d_img->capture_texture) {
            ID3D11Device *d3d_dev = nullptr;
            const_cast<ID3D11Texture2D *>(d3d_img->capture_texture.get())->GetDevice(&d3d_dev);
            if (d3d_dev) {
              IDXGIDevice *dxgi_dev = nullptr;
              if (SUCCEEDED(d3d_dev->QueryInterface(__uuidof(IDXGIDevice), (void **) &dxgi_dev)) && dxgi_dev) {
                IDXGIAdapter *adapter = nullptr;
                if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
                  DXGI_ADAPTER_DESC desc {};
                  if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    pyrowave_luid luid {};
                    static_assert(sizeof(luid.luid) == sizeof(desc.AdapterLuid), "LUID size mismatch");
                    std::memcpy(luid.luid, &desc.AdapterLuid, sizeof(luid.luid));
                    res = pyrowave_create_device_by_compat(
                      0,
                      0,
                      nullptr,
                      nullptr,
                      &luid,
                      VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
                      &device
                    );
                  }
                  adapter->Release();
                }
                dxgi_dev->Release();
              }
              d3d_dev->Release();
            }
          }
        }
      }
#else
      (void) first_img;
#endif

      if (res != PYROWAVE_SUCCESS || !device) {
        res = pyrowave_create_default_device(&device);
        if (res != PYROWAVE_SUCCESS || !device) {
          BOOST_LOG(error) << "[VIPLE-PYROWAVE] Failed to create PyroWave Vulkan device (err=" << (int) res << ")";
          return false;
        }
      }

      pyrowave_device_set_queue_type(device, VK_QUEUE_COMPUTE_BIT);

      pyrowave_encoder_create_info enc_info {};
      enc_info.device = device;
      enc_info.width = config.width;
      enc_info.height = config.height;
      enc_info.chroma = (config.chromaSamplingType == 1) ?
                          PYROWAVE_CHROMA_SUBSAMPLING_444 :
                          PYROWAVE_CHROMA_SUBSAMPLING_420;

      res = pyrowave_encoder_create(&enc_info, &encoder);
      if (res != PYROWAVE_SUCCESS || !encoder) {
        BOOST_LOG(error) << "[VIPLE-PYROWAVE] Failed to create PyroWave encoder "
                         << config.width << "x" << config.height
                         << " chroma=" << (config.chromaSamplingType == 1 ? "444" : "420")
                         << " (err=" << (int) res << ")";
        return false;
      }

      BOOST_LOG(info) << "[VIPLE-PYROWAVE] Initialized PyroWave encoder: "
                      << config.width << "x" << config.height
                      << " @ " << config.framerate << " fps, "
                      << config.bitrate << " kbps ("
                      << (max_bitstream_size / 1024) << " KB/frame), "
                      << (config.chromaSamplingType == 1 ? "YUV444 " : "YUV420 ")
                      << colorspace.bit_depth << "-bit";
      return true;
    }

#ifdef _WIN32
    static VkFormat map_dxgi_to_vk_format(DXGI_FORMAT fmt) {
      switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
          return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
          return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        default:
          return VK_FORMAT_UNDEFINED;
      }
    }

    bool encode_d3d11_zero_copy(platf::dxgi::img_d3d_t &d3d_img) {
      // Garbage-collect expired cache entries
      for (auto it = d3d_cache.begin(); it != d3d_cache.end();) {
        if (it->second.img_weak.expired()) {
          it = d3d_cache.erase(it);
        } else {
          ++it;
        }
      }

      if (!d3d_img.capture_texture || !d3d_img.encoder_texture_handle) {
        return false;
      }

      VkFormat vk_fmt = map_dxgi_to_vk_format(d3d_img.format);
      if (vk_fmt == VK_FORMAT_UNDEFINED) {
        return false;
      }

      auto &entry = d3d_cache[d3d_img.id];
      if (!entry.image ||
          entry.capture_texture_p != d3d_img.capture_texture.get() ||
          entry.orig_tex_handle != d3d_img.encoder_texture_handle ||
          entry.orig_fence_handle != d3d_img.fence_shared_handle) {
        entry.reset();

        HANDLE dup_tex = nullptr;
        if (!DuplicateHandle(
              GetCurrentProcess(),
              d3d_img.encoder_texture_handle,
              GetCurrentProcess(),
              &dup_tex,
              0,
              FALSE,
              DUPLICATE_SAME_ACCESS
            )) {
          BOOST_LOG(error) << "[VIPLE-PYROWAVE] DuplicateHandle failed for D3D11 texture";
          return false;
        }

        VkImageCreateInfo vk_img_info {};
        vk_img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        vk_img_info.imageType = VK_IMAGE_TYPE_2D;
        vk_img_info.format = vk_fmt;
        vk_img_info.extent.width = (uint32_t) d3d_img.width;
        vk_img_info.extent.height = (uint32_t) d3d_img.height;
        vk_img_info.extent.depth = 1;
        vk_img_info.mipLevels = 1;
        vk_img_info.arrayLayers = 1;
        vk_img_info.samples = VK_SAMPLE_COUNT_1_BIT;
        vk_img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        vk_img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        vk_img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk_img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        pyrowave_image_create_info img_ci {};
        img_ci.device = device;
        img_ci.external_handle = (pyrowave_os_handle) (uintptr_t) dup_tex;
        img_ci.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        img_ci.image_create_info = &vk_img_info;

        if (pyrowave_image_create(&img_ci, &entry.image) != PYROWAVE_SUCCESS || !entry.image) {
          CloseHandle(dup_tex);
          BOOST_LOG(warning) << "[VIPLE-PYROWAVE] Failed to import D3D11 shared texture into Vulkan";
          return false;
        }

        if (d3d_img.fence_shared_handle) {
          HANDLE dup_fence = nullptr;
          if (DuplicateHandle(
                GetCurrentProcess(),
                d3d_img.fence_shared_handle,
                GetCurrentProcess(),
                &dup_fence,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS
              )) {
            pyrowave_sync_object_create_info sync_ci {};
            sync_ci.device = device;
            sync_ci.external_handle = (pyrowave_os_handle) (uintptr_t) dup_fence;
            sync_ci.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            sync_ci.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
            sync_ci.import_flags = 0;

            if (pyrowave_sync_object_create(&sync_ci, &entry.sync) != PYROWAVE_SUCCESS || !entry.sync) {
              CloseHandle(dup_fence);
              entry.reset();
              BOOST_LOG(warning) << "[VIPLE-PYROWAVE] Failed to import D3D11 shared fence into Vulkan";
              return false;
            }
          }
        }

        entry.capture_texture_p = d3d_img.capture_texture.get();
        entry.orig_tex_handle = d3d_img.encoder_texture_handle;
        entry.orig_fence_handle = d3d_img.fence_shared_handle;
        entry.vk_format = vk_fmt;
        entry.width = (uint32_t) d3d_img.width;
        entry.height = (uint32_t) d3d_img.height;
        entry.img_weak = d3d_img.weak_from_this();
      }

      pyrowave_image_view view {};
      if (pyrowave_image_get_image_view(
            entry.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            &view
          ) != PYROWAVE_SUCCESS) {
        return false;
      }

      bool is_hdr = video::colorspace_is_hdr(colorspace);
      pyrowave_scaled_encode_info scale_info {};
      scale_info.view = view;
      if (d3d_img.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        scale_info.input_color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
      } else if (d3d_img.format == DXGI_FORMAT_R10G10B10A2_UNORM && is_hdr) {
        scale_info.input_color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
      } else {
        scale_info.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
      scale_info.output_color_space = is_hdr ?
                                        VK_COLOR_SPACE_HDR10_ST2084_EXT :
                                        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      scale_info.intermediate_plane_format = (colorspace.bit_depth == 10) ?
                                               VK_FORMAT_R16_UNORM :
                                               VK_FORMAT_R8_UNORM;
      scale_info.ycbcr_chroma_midpoint = (colorspace.bit_depth == 10) ?
                                           (512.0f / 1023.0f) :
                                           (128.0f / 255.0f);
      scale_info.force_linear_filtering = false;
      scale_info.skip_dither = (colorspace.bit_depth == 10);
      scale_info.crop_rect = nullptr;
      scale_info.ycbcr_range = colorspace.full_range ?
                                 VK_SAMPLER_YCBCR_RANGE_ITU_FULL :
                                 VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
      scale_info.ycbcr_range_bit_depth = (uint32_t) colorspace.bit_depth;

      pyrowave_gpu_external_reference ext_ref {};
      ext_ref.image = entry.image;
      ext_ref.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;

      uint64_t fence_val = d3d_img.fence_value.load(std::memory_order_acquire);
      pyrowave_gpu_sync_operation acquire_op {};
      acquire_op.images = &ext_ref;
      acquire_op.num_images = 1;
      if (entry.sync && fence_val > 0) {
        acquire_op.sync.semaphore = pyrowave_sync_object_get_semaphore(entry.sync);
        acquire_op.sync.value = fence_val;
      } else {
        acquire_op.sync.semaphore = VK_NULL_HANDLE;
        acquire_op.sync.value = 0;
      }

      pyrowave_rate_control rc {};
      rc.maximum_bitstream_size = max_bitstream_size;

      pyrowave_encoder_set_ycbcr_scaling_factor(encoder, 1.0f);
      pyrowave_result res = pyrowave_encoder_encode_gpu_scaled(
        encoder,
        &acquire_op,
        nullptr,
        &scale_info,
        &rc
      );
      if (res != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "[VIPLE-PYROWAVE] pyrowave_encoder_encode_gpu_scaled failed (err=" << (int) res << ")";
        return false;
      }

      has_encoded_frame = true;
      return true;
    }
#endif

    bool ensure_cpu_frames(int src_w, int src_h) {
      AVPixelFormat dst_fmt = AV_PIX_FMT_YUV420P;
      if (config.chromaSamplingType == 1) {
        dst_fmt = (colorspace.bit_depth == 10) ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
      } else {
        dst_fmt = (colorspace.bit_depth == 10) ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
      }

      if (!sw_frame) {
        sw_frame.reset(av_frame_alloc());
        if (!sw_frame) {
          return false;
        }
        sw_frame->width = config.width;
        sw_frame->height = config.height;
        sw_frame->format = dst_fmt;
        if (av_frame_get_buffer(sw_frame.get(), 32) < 0) {
          return false;
        }
        auto av_cs = video::avcodec_colorspace_from_sunshine_colorspace(colorspace);
        sw_frame->color_range = av_cs.range;
        ptrdiff_t linesize[4] = {
          sw_frame->linesize[0],
          sw_frame->linesize[1],
          sw_frame->linesize[2],
          sw_frame->linesize[3]
        };
        av_image_fill_black(
          sw_frame->data,
          linesize,
          dst_fmt,
          sw_frame->color_range,
          sw_frame->width,
          sw_frame->height
        );
      }

      if (src_w > 0 && src_h > 0 && (!sws || sws_src_w != src_w || sws_src_h != src_h)) {
        sws_in_frame.reset(av_frame_alloc());
        if (!sws_in_frame) {
          return false;
        }
        sws_in_frame->width = src_w;
        sws_in_frame->height = src_h;
        sws_in_frame->format = AV_PIX_FMT_BGR0;

        sws.reset(sws_alloc_context());
        if (!sws) {
          return false;
        }

        AVDictionary *options = nullptr;
        av_dict_set_int(&options, "srcw", src_w, 0);
        av_dict_set_int(&options, "srch", src_h, 0);
        av_dict_set_int(&options, "src_format", AV_PIX_FMT_BGR0, 0);
        av_dict_set_int(&options, "dstw", config.width, 0);
        av_dict_set_int(&options, "dsth", config.height, 0);
        av_dict_set_int(&options, "dst_format", dst_fmt, 0);
        av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
        av_dict_set_int(&options, "threads", config::video.min_threads, 0);

        int status = av_opt_set_dict(sws.get(), &options);
        av_dict_free(&options);
        if (status < 0 || sws_init_context(sws.get(), nullptr, nullptr) < 0) {
          BOOST_LOG(error) << "[VIPLE-PYROWAVE] Failed to initialize SWS fallback context";
          sws.reset();
          return false;
        }

        auto av_cs = video::avcodec_colorspace_from_sunshine_colorspace(colorspace);
        sws_setColorspaceDetails(
          sws.get(),
          sws_getCoefficients(SWS_CS_DEFAULT),
          0,
          sws_getCoefficients(av_cs.software_format),
          av_cs.range - 1,
          0,
          1 << 16,
          1 << 16
        );

        sws_src_w = src_w;
        sws_src_h = src_h;
      }

      return true;
    }

    bool encode_cpu_fallback(const platf::img_t *img) {
      int src_w = img ? img->width : config.width;
      int src_h = img ? img->height : config.height;
      if (!ensure_cpu_frames(src_w, src_h)) {
        return false;
      }

      if (img && img->data && sws) {
        sws_in_frame->data[0] = img->data;
        sws_in_frame->linesize[0] = img->row_pitch;
        if (sws_scale_frame(sws.get(), sw_frame.get(), sws_in_frame.get()) < 0) {
          BOOST_LOG(error) << "[VIPLE-PYROWAVE] sws_scale_frame failed in CPU fallback";
          return false;
        }
      }

      pyrowave_cpu_buffer cpu_buf {};
      cpu_buf.width = config.width;
      cpu_buf.height = config.height;
      if (config.chromaSamplingType == 1) {
        cpu_buf.format = (colorspace.bit_depth == 10) ?
                           PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10 :
                           PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
      } else {
        cpu_buf.format = (colorspace.bit_depth == 10) ?
                           PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10 :
                           PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
      }

      for (int p = 0; p < 3; ++p) {
        int plane_h = (p > 0 && config.chromaSamplingType == 0) ? (config.height / 2) : config.height;
        cpu_buf.data[p] = sw_frame->data[p];
        cpu_buf.row_stride_in_bytes[p] = (size_t) sw_frame->linesize[p];
        cpu_buf.plane_size_in_bytes[p] = (size_t) sw_frame->linesize[p] * (size_t) plane_h;
      }

      pyrowave_rate_control rc {};
      rc.maximum_bitstream_size = max_bitstream_size;

      pyrowave_result res = pyrowave_encoder_encode_cpu(encoder, &cpu_buf, &rc);
      if (res != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "[VIPLE-PYROWAVE] pyrowave_encoder_encode_cpu failed (err=" << (int) res << ")";
        return false;
      }

      has_encoded_frame = true;
      return true;
    }
  };

  pyrowave_encode_session_t::~pyrowave_encode_session_t() = default;

  bool pyrowave_encode_session_t::init(
    platf::display_t *disp,
    const video::config_t &config,
    int capture_width,
    int capture_height,
    const video::sunshine_colorspace_t &colorspace
  ) {
    (void) disp;
    impl_ = std::make_unique<impl_t>();
    impl_->config = config;
    impl_->colorspace = colorspace;
    impl_->capture_width = capture_width;
    impl_->capture_height = capture_height;
    impl_->update_rate_control();
    return true;
  }

  int pyrowave_encode_session_t::convert(platf::img_t &img) {
    if (!impl_) {
      return -1;
    }
    if (!impl_->ensure_device_and_encoder(&img)) {
      return -1;
    }

#ifdef _WIN32
    if (auto d3d_img = dynamic_cast<platf::dxgi::img_d3d_t *>(&img)) {
      if (impl_->encode_d3d11_zero_copy(*d3d_img)) {
        return 0;
      }
      // Do not pass d3d_img to encode_cpu_fallback because d3d_img->data is ID3D11Texture2D*
      return impl_->encode_cpu_fallback(nullptr) ? 0 : -1;
    }
#endif

    return impl_->encode_cpu_fallback(&img) ? 0 : -1;
  }

  void pyrowave_encode_session_t::request_idr_frame() {
    // PyroWave is pure intra-frame wavelet coding: every frame is self-contained.
  }

  void pyrowave_encode_session_t::request_normal_frame() {
  }

  void pyrowave_encode_session_t::invalidate_ref_frames(int64_t first_frame, int64_t last_frame) {
    (void) first_frame;
    (void) last_frame;
  }

  bool pyrowave_encode_session_t::request_intra_refresh(int frameCnt) {
    (void) frameCnt;
    // Every PyroWave frame is already an intra frame
    return true;
  }

  void pyrowave_encode_session_t::reconfigure_bitrate(int bitrate_kbps, int framerate) {
    if (!impl_) {
      return;
    }
    impl_->config.bitrate = bitrate_kbps;
    if (framerate > 0) {
      impl_->config.framerate = framerate;
    }
    impl_->update_rate_control();
    BOOST_LOG(info) << "[VIPLE-PYROWAVE] Bitrate updated to " << bitrate_kbps
                    << " kbps (" << (impl_->max_bitstream_size / 1024) << " KB/frame)";
  }

  void pyrowave_encode_session_t::reconfigure_fps(int framerate) {
    if (!impl_ || framerate <= 0) {
      return;
    }
    impl_->config.framerate = framerate;
    impl_->update_rate_control();
    BOOST_LOG(info) << "[VIPLE-PYROWAVE] Framerate updated to " << framerate
                    << " fps (" << (impl_->max_bitstream_size / 1024) << " KB/frame)";
  }

  encoded_frame_t pyrowave_encode_session_t::encode_frame(int64_t frame_nr) {
    encoded_frame_t out {};
    out.frame_index = frame_nr;
    // PyroWave 每一幀都是獨立的 intra 幀，一律標成 IDR。只標第 1 幀的話，第 1 幀一丟
    // client 就永遠卡在「Waiting for IDR frame」（2.0.23 首次實測就是這樣）。
    out.idr = true;
    out.after_ref_frame_invalidation = false;

    if (!impl_ || !impl_->encoder) {
      return out;
    }

    // If convert() was skipped on timeout (repeated frame), re-encode a fallback frame
    // or re-packetize the last encoded frame. Notice pyrowave_encoder_packetize_with_padding
    // can be called again on the current frame context, or we can re-encode if needed.
    if (!impl_->has_encoded_frame) {
      if (!impl_->encode_cpu_fallback(nullptr)) {
        return out;
      }
    }

    const size_t packet_size = (size_t) std::max(256, impl_->config.packetSize);
    const size_t payload_blocksize = packet_size - sizeof(NV_VIDEO_PACKET);
    const size_t frame_header_size = impl_->config.vrProfile ? 24 : 8;

    size_t num_packets = 0;
    pyrowave_result res = pyrowave_encoder_compute_num_packets_with_padding(
      impl_->encoder,
      payload_blocksize,
      frame_header_size,
      &num_packets
    );
    if (res != PYROWAVE_SUCCESS || num_packets == 0) {
      BOOST_LOG(error) << "[VIPLE-PYROWAVE] compute_num_packets_with_padding failed (err=" << (int) res << ")";
      return out;
    }

    if (impl_->packet_descs.size() < num_packets) {
      impl_->packet_descs.resize(num_packets);
    }

    // packetize 不檢查輸出大小（pyrowave_encoder.cpp 裡是 (void)size），單一區塊又可能大於
    // payload_blocksize，所以不能用「封包數 × payload」估。上限＝encoder 的原始位元流容量
    // （所有區塊加總）＋8 B 序列 header。
    const void *mapped_bits = nullptr;
    const void *mapped_meta = nullptr;
    size_t mapped_bits_size = 0;
    size_t mapped_meta_size = 0;
    res = pyrowave_encoder_get_mapped_raw_bitstream(impl_->encoder, &mapped_bits, &mapped_bits_size, &mapped_meta, &mapped_meta_size);
    if (res != PYROWAVE_SUCCESS || mapped_bits_size == 0) {
      BOOST_LOG(error) << "[VIPLE-PYROWAVE] get_mapped_raw_bitstream failed (err=" << (int) res << ")";
      return out;
    }
    const size_t max_raw_size = std::max(num_packets * payload_blocksize, mapped_bits_size) + 4096;
    if (impl_->raw_bitstream_buf.size() < max_raw_size) {
      impl_->raw_bitstream_buf.resize(max_raw_size);
    }

    size_t out_packets = 0;
    res = pyrowave_encoder_packetize_with_padding(
      impl_->encoder,
      impl_->packet_descs.data(),
      payload_blocksize,
      frame_header_size,
      &out_packets,
      impl_->raw_bitstream_buf.data(),
      impl_->raw_bitstream_buf.size()
    );
    if (res != PYROWAVE_SUCCESS || out_packets == 0) {
      BOOST_LOG(error) << "[VIPLE-PYROWAVE] packetize_with_padding failed (err=" << (int) res << ")";
      return out;
    }

    // packetize 寫出的位元流本身就是連續的（序列 header＋各區塊依序排列），packet_descs
    // 只是在上面切範圍。單一 32x32 區塊可能比一個 RTP payload 還大（179 Mbps 實測有
    // 1596 B > 1376 B），所以不能照 slot 對齊填充／截斷，整段原樣送出，由 stream.cpp
    // 切 RTP；client 把整幀接回連續 buffer 再一次 push_packet。
    const auto &last = impl_->packet_descs[out_packets - 1];
    const size_t total_size = last.offset + last.size;
    if (total_size > impl_->raw_bitstream_buf.size()) {
      BOOST_LOG(error) << "[VIPLE-PYROWAVE] packetize produced " << total_size
                       << " B, exceeds buffer " << impl_->raw_bitstream_buf.size();
      return out;
    }
    out.data.assign(impl_->raw_bitstream_buf.begin(), impl_->raw_bitstream_buf.begin() + total_size);

    return out;
  }

  std::unique_ptr<pyrowave_encode_session_t> make_encode_session(
    platf::display_t *disp,
    const video::config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::encode_device_t> encode_device
  ) {
    if (!encode_device) {
      return nullptr;
    }
    auto session = std::make_unique<pyrowave_encode_session_t>();
    if (!session->init(disp, config, width, height, encode_device->colorspace)) {
      return nullptr;
    }
    return session;
  }

  int encode_pyrowave(
    int64_t frame_nr,
    pyrowave_encode_session_t &session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    const video::vr_frame_meta_t *vr_meta
  ) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "[VIPLE-PYROWAVE] Encoder returned empty packet at frame " << frame_nr;
      return -1;
    }

    auto packet = std::make_unique<video::packet_raw_generic>(
      std::move(encoded_frame.data),
      encoded_frame.frame_index,
      encoded_frame.idr
    );
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    if (vr_meta) {
      packet->vr_meta = *vr_meta;
    }
    packets->raise(std::move(packet));
    return 0;
  }

}  // namespace pyrowave
