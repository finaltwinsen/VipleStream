/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    // §REVIEW-1103：client 給的 surroundParams（例如 coupled > streams）會讓 opus 回 NULL，
    // 接著 ctl(NULL) 就讓 server 崩潰；建立失敗就結束這條音訊串流。
    if (!opus) {
      BOOST_LOG(error) << "Couldn't create Opus encoder: "sv << stream.channelCount << " channels, "sv
                       << stream.streams << " streams, "sv << stream.coupledStreams << " coupled"sv;
      packets->stop();

      return;
    }

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));

      // §Q.r1 diag: 確認 encodeThread 有在 push packet
      {
        static bool encode_once = false;
        if (!encode_once) {
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q.r1 encodeThread: first audio packet pushed"
                          << " bytes=" << bytes;
          encode_once = true;
        }
      }
    }
  }

  // §AUD-MON：擷取健康檢查的參數。
  //   上游的擷取在主機沒出聲時本來就不送封包（沒有 CONTINUOUS_AUDIO 時 WASAPI 直接逾時），client 光看
  //   「多久沒收到音訊」分不出「沒聲音」和「擷取壞了」。這裡改在 server 端比對：播放裝置的音量計顯示
  //   有在出聲、擷取卻連續幾秒都拿不到有聲音的樣本，才算異常。
  constexpr auto kMonMeterInterval = 250ms;  ///< 讀音量計的間隔
  constexpr auto kMonWindow = 1s;  ///< 一個判定窗
  constexpr float kMonPlayingPeak = 0.01f;  ///< 音量計峰值 ≥ −40 dBFS 算「有在出聲」
  constexpr float kMonAudibleSample = 1e-4f;  ///< 擷取樣本絕對值超過這個算「拿到聲音」（比上面低 40 dB）
  constexpr int kMonStallWindows = 5;  ///< 連續幾個判定窗異常才重建擷取
  constexpr int kMonReportAfterTries = 3;  ///< 連續重建幾次都沒好才通知 client
  constexpr int kMonRetryEvery = 6;  ///< 通知之後，每隔幾次異常（約 30 秒）再重建一次

  static bool has_audible_sample(const std::vector<float> &buf) {
    for (auto v : buf) {
      if (v > kMonAudibleSample || v < -kMonAudibleSample) {
        return true;
      }
    }
    return false;
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto status_queue = mail->queue<int>(mail::audio_status);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event, &status_queue]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;
      status_queue->raise((int) status_e::init_failed);

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    auto reinit_mic = [&]() {
      mic.reset();
      do {
        mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
        if (!mic) {
          BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
        }
      } while (!mic && !shutdown_event->view(5s));
    };

    // §AUD-MON 狀態
    const bool mon_supported = mic->render_peak().has_value();
    if (mon_supported) {
      BOOST_LOG(info) << "[VIPLE-AUD-MON] capture health check active"sv;
      status_queue->raise((int) status_e::monitoring);
    } else {
      BOOST_LOG(info) << "[VIPLE-AUD-MON] no render peak meter on this platform, capture health check disabled"sv;
    }
    auto mon_next_meter = std::chrono::steady_clock::now();
    auto mon_window_start = mon_next_meter;
    bool mon_win_playing = false;  // 這個判定窗內音量計有沒有看到聲音
    bool mon_win_captured = false;  // 這個判定窗內擷取有沒有拿到聲音
    float mon_win_peak = 0.0f;
    int mon_stalled_windows = 0;
    int mon_tries = 0;  // 連續因異常重建擷取的次數（擷取拿到聲音就歸零）
    bool mon_reported = false;  // 已經通知 client stalled

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);

      // §AUD-MON：每次迭代都要跑（逾時也是），所以放在 switch 之前。
      // 主機沒出聲時 sample() 每個裝置週期（Windows 共用模式約 10 ms）逾時一次，迴圈不會卡住；
      // 判定只看時間（250 ms 讀音量計、1 s 判定窗），不數逾時次數，所以逾時週期長短不影響結果。
      // （§REVIEW-1103 之前 Windows 的逾時誤算成約 100 ms，判定一樣正確。）
      if (mon_supported && mic) {
        auto now = std::chrono::steady_clock::now();
        if (status == platf::capture_e::ok && !mon_win_captured && has_audible_sample(sample_buffer)) {
          mon_win_captured = true;
        }
        if (now >= mon_next_meter) {
          mon_next_meter = now + kMonMeterInterval;
          if (auto peak = mic->render_peak(); peak && *peak >= kMonPlayingPeak) {
            mon_win_playing = true;
            mon_win_peak = std::max(mon_win_peak, *peak);
          }
        }
        if (now - mon_window_start >= kMonWindow) {
          bool do_reinit = false;
          if (mon_win_captured) {
            if (mon_reported) {
              BOOST_LOG(info) << "[VIPLE-AUD-MON] capture recovered"sv;
              status_queue->raise((int) status_e::recovered);
              mon_reported = false;
            }
            mon_stalled_windows = 0;
            mon_tries = 0;
          } else if (mon_win_playing) {
            if (++mon_stalled_windows >= kMonStallWindows) {
              mon_stalled_windows = 0;
              ++mon_tries;
              BOOST_LOG(warning) << "[VIPLE-AUD-MON] host is playing audio (peak "sv << mon_win_peak
                                 << ") but capture got nothing for "sv << kMonStallWindows << " s (try "sv << mon_tries << ')';
              if (mon_tries >= kMonReportAfterTries && !mon_reported) {
                BOOST_LOG(error) << "[VIPLE-AUD-MON] capture still silent after "sv << mon_tries << " re-initializations, notifying client"sv;
                status_queue->raise((int) status_e::stalled);
                mon_reported = true;
              }
              do_reinit = mon_tries <= kMonReportAfterTries || (mon_tries % kMonRetryEvery) == 0;
            }
          } else {
            // 主機沒出聲：什麼都不判斷（重建次數保留，靜音不算恢復）
            mon_stalled_windows = 0;
          }
          mon_window_start = now;
          mon_win_playing = false;
          mon_win_captured = false;
          mon_win_peak = 0.0f;

          if (do_reinit) {
            BOOST_LOG(info) << "[VIPLE-AUD-MON] re-initializing audio capture"sv;
            reinit_mic();
            if (!mic) {
              return;  // shutdown 期間沒重建成功
            }
            mon_window_start = std::chrono::steady_clock::now();
            continue;
          }
        }
      }

      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          reinit_mic();
          if (!mic) {
            return;
          }
          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
