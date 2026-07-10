// A0 스파이크: 재생 중 audiomixer 출력 채널수(버스) 라이브 전환 + mix-matrix 라우팅 검증
//
// 검증 대상 (플랜 §2 P-A1의 최대 리스크):
//   1. audiomixer sink 패드(GstAudioAggregatorConvertPad)의 converter-config mix-matrix로
//      소스 채널 → 버스 채널 라우팅이 되는가
//   2. PLAYING 중 출력 capsfilter의 채널수를 바꾸면(2ch↔8ch) 파이프라인이 에러 없이
//      재협상되는가 (플레이어의 DoAudioSinkSwap과 동일한 IDLE 프로브 + sink 교체 패턴)
//   3. WASAPI 다채널 디바이스가 8ch(fallback mask)를 수용하는가
//   4. (ASIO 연결 시) asiosink가 unpositioned N채널을 수용하는가
//
// 사용법: spike_mixcaps.exe [wasapi 디바이스 이름 부분문자열]
// 출력: 각 단계 결과 + 마지막 줄 "SPIKE: PASS" / "SPIKE: FAIL"

#include <gst/audio/audio.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace {

GMainLoop* g_loop = nullptr;
std::atomic<int> g_error_count{0};
std::atomic<guint64> g_sink_buffers{0};

// map[src_ch] = 버스 채널 인덱스 (-1 = 뮤트). 행=출력, 열=입력.
GstStructure* MakeMatrixConfig(int src_ch, int out_ch, const std::vector<int>& map) {
  GValue matrix = G_VALUE_INIT;
  g_value_init(&matrix, GST_TYPE_ARRAY);
  for (int o = 0; o < out_ch; ++o) {
    GValue row = G_VALUE_INIT;
    g_value_init(&row, GST_TYPE_ARRAY);
    for (int s = 0; s < src_ch; ++s) {
      GValue v = G_VALUE_INIT;
      g_value_init(&v, G_TYPE_FLOAT);
      const bool hit = s < static_cast<int>(map.size()) && map[s] == o;
      g_value_set_float(&v, hit ? 1.0f : 0.0f);
      gst_value_array_append_value(&row, &v);
      g_value_unset(&v);
    }
    gst_value_array_append_value(&matrix, &row);
    g_value_unset(&row);
  }
  GstStructure* st = gst_structure_new_empty("converter-config");
  gst_structure_set_value(st, GST_AUDIO_CONVERTER_OPT_MIX_MATRIX, &matrix);
  g_value_unset(&matrix);
  return st;
}

void SetPadMatrix(GstPad* pad, int src_ch, int out_ch, const std::vector<int>& map) {
  GstStructure* st = MakeMatrixConfig(src_ch, out_ch, map);
  g_object_set(pad, "converter-config", st, nullptr);
  gst_structure_free(st);
}

// 버스(믹서 출력) caps. positioned=true면 표준 fallback 마스크, 아니면 unpositioned(mask=0)
GstCaps* MakeBusCaps(int channels, bool positioned) {
  GstCaps* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                                      G_TYPE_INT, 48000, "channels", G_TYPE_INT, channels,
                                      "layout", G_TYPE_STRING, "interleaved", nullptr);
  guint64 mask = 0;
  if (positioned && channels <= 8) mask = gst_audio_channel_get_fallback_mask(channels);
  gst_caps_set_simple(caps, "channel-mask", GST_TYPE_BITMASK, mask, nullptr);
  return caps;
}

gboolean OnBusMessage(GstBus*, GstMessage* msg, gpointer) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    GError* err = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_error(msg, &err, &dbg);
    printf("[BUS ERROR] %s: %s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
           err ? err->message : "?");
    if (dbg) printf("  debug: %s\n", dbg);
    if (err) g_error_free(err);
    g_free(dbg);
    g_error_count++;
  }
  return G_SOURCE_CONTINUE;
}

struct Ctx {
  GstElement* pipeline = nullptr;
  GstElement* amix = nullptr;
  GstElement* bus_caps = nullptr;   // amix 직후 출력 capsfilter
  GstElement* tail = nullptr;       // 출력단 audioresample (sink 교체 지점)
  GstElement* sink = nullptr;
  GstPad* sine_pad = nullptr;       // 사인 브랜치의 amix 요청 패드
  GstPad* silence_pad = nullptr;    // 무음 앵커의 amix 요청 패드
  std::string wasapi_device;        // 선택된 wasapi 디바이스 id
  std::string asio_clsid;           // 발견된 asio CLSID ("" = 없음)
  int wasapi_ch = 2;
  int asio_ch = 0;                  // asio 드라이버 보고 채널수
};

// 디바이스 열람: 가장 채널 많은 wasapi 디바이스 + (있으면) asio CLSID
void PickDevices(Ctx* ctx, const char* name_filter) {
  auto scan = [&](const char* factory, auto fn) {
    GstDeviceProviderFactory* f = gst_device_provider_factory_find(factory);
    GstDeviceProvider* p = f ? gst_device_provider_factory_get(f) : nullptr;
    if (f) gst_object_unref(f);
    if (!p) return;
    gst_device_provider_start(p);
    GList* list = gst_device_provider_get_devices(p);
    for (GList* it = list; it; it = it->next) {
      GstDevice* dev = GST_DEVICE(it->data);
      if (!gst_device_has_classes(dev, "Audio/Sink")) continue;
      fn(dev);
    }
    g_list_free_full(list, gst_object_unref);
    gst_device_provider_stop(p);
    gst_object_unref(p);
  };

  auto caps_channels = [](GstDevice* dev) {
    GstCaps* caps = gst_device_get_caps(dev);
    if (!caps) return 0;
    int channels = 0;
    for (guint i = 0; i < gst_caps_get_size(caps); i++) {
      const GValue* v = gst_structure_get_value(gst_caps_get_structure(caps, i), "channels");
      if (!v) continue;
      if (G_VALUE_HOLDS_INT(v)) channels = MAX(channels, g_value_get_int(v));
      else if (GST_VALUE_HOLDS_INT_RANGE(v)) channels = MAX(channels, gst_value_get_int_range_max(v));
    }
    gst_caps_unref(caps);
    return channels;
  };

  int best_ch = 0;
  scan("wasapi2deviceprovider", [&](GstDevice* dev) {
    gchar* name = gst_device_get_display_name(dev);
    GstStructure* props = gst_device_get_properties(dev);
    const gchar* id = props ? gst_structure_get_string(props, "device.id") : nullptr;
    const int ch = caps_channels(dev);
    printf("[wasapi] %s — %dch\n", name ? name : "?", ch);
    const bool name_ok = !name_filter || (name && strstr(name, name_filter));
    if (id && name_ok && ch > best_ch) {
      best_ch = ch;
      ctx->wasapi_device = id;
      ctx->wasapi_ch = MIN(ch, 8);
    }
    if (props) gst_structure_free(props);
    g_free(name);
  });
  scan("asiodeviceprovider", [&](GstDevice* dev) {
    gchar* name = gst_device_get_display_name(dev);
    GstStructure* props = gst_device_get_properties(dev);
    const gchar* clsid = props ? gst_structure_get_string(props, "device.clsid") : nullptr;
    const int ch = caps_channels(dev);
    printf("[asio] %s — %dch (clsid %s)\n", name ? name : "?", ch, clsid ? clsid : "?");
    if (clsid && ctx->asio_clsid.empty()) {
      ctx->asio_clsid = clsid;
      ctx->asio_ch = ch;
    }
    if (props) gst_structure_free(props);
    g_free(name);
  });
}

bool BuildPipeline(Ctx* ctx) {
  ctx->pipeline = gst_pipeline_new("spike");
  {
    GstClock* sysclock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(ctx->pipeline), sysclock);
    gst_object_unref(sysclock);
  }

  // 사인 브랜치 (덱 오디오 브랜치 모사: 소스 원본 채널 유지, 변환은 믹서 패드 matrix)
  GstElement* sine = gst_element_factory_make("audiotestsrc", "sine");
  g_object_set(sine, "wave", 0 /* sine */, "freq", 440.0, "is-live", TRUE, "volume", 0.3,
               nullptr);
  GstElement* sine_caps = gst_element_factory_make("capsfilter", "sine_caps");
  {
    GstCaps* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                                        G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, "layout",
                                        G_TYPE_STRING, "interleaved", nullptr);
    g_object_set(sine_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }

  // 무음 앵커 (모노 고정 + 제로 matrix — 버스 채널수가 바뀌어도 caps 불변)
  GstElement* silence = gst_element_factory_make("audiotestsrc", "silence");
  g_object_set(silence, "wave", 4 /* silence */, "is-live", TRUE, nullptr);
  GstElement* silence_caps = gst_element_factory_make("capsfilter", "silence_caps");
  {
    GstCaps* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate",
                                        G_TYPE_INT, 48000, "channels", G_TYPE_INT, 1, "layout",
                                        G_TYPE_STRING, "interleaved", nullptr);
    g_object_set(silence_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }

  ctx->amix = gst_element_factory_make("audiomixer", "amix");
  ctx->bus_caps = gst_element_factory_make("capsfilter", "bus_caps");
  {
    GstCaps* caps = MakeBusCaps(2, /*positioned=*/true);
    g_object_set(ctx->bus_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
  }
  GstElement* conv = gst_element_factory_make("audioconvert", "conv_out");
  ctx->tail = gst_element_factory_make("audioresample", "ares_out");
  ctx->sink = gst_element_factory_make("wasapi2sink", "asink");
  if (!ctx->sink) {
    printf("wasapi2sink unavailable\n");
    return false;
  }
  if (!ctx->wasapi_device.empty())
    g_object_set(ctx->sink, "device", ctx->wasapi_device.c_str(), nullptr);

  gst_bin_add_many(GST_BIN(ctx->pipeline), sine, sine_caps, silence, silence_caps, ctx->amix,
                   ctx->bus_caps, conv, ctx->tail, ctx->sink, nullptr);
  if (!gst_element_link(sine, sine_caps) || !gst_element_link(silence, silence_caps) ||
      !gst_element_link_many(ctx->amix, ctx->bus_caps, conv, ctx->tail, ctx->sink, nullptr)) {
    printf("link failed (output stage)\n");
    return false;
  }

  auto link_to_mixer = [&](GstElement* branch_tail) -> GstPad* {
    GstPad* src = gst_element_get_static_pad(branch_tail, "src");
    GstPad* pad = gst_element_request_pad_simple(ctx->amix, "sink_%u");
    const bool ok = gst_pad_link(src, pad) == GST_PAD_LINK_OK;
    gst_object_unref(src);
    if (!ok) {
      printf("link to mixer failed\n");
      return nullptr;
    }
    return pad;
  };
  ctx->sine_pad = link_to_mixer(sine_caps);
  ctx->silence_pad = link_to_mixer(silence_caps);
  if (!ctx->sine_pad || !ctx->silence_pad) return false;

  // 초기 matrix: 사인 스테레오 → 버스 0,1 / 무음 모노 → 없음
  SetPadMatrix(ctx->sine_pad, 2, 2, {0, 1});
  SetPadMatrix(ctx->silence_pad, 1, 2, {-1});

  // sink 버퍼 카운터 (흐름 확인)
  GstPad* sinkpad = gst_element_get_static_pad(ctx->sink, "sink");
  gst_pad_add_probe(
      sinkpad, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo*, gpointer) -> GstPadProbeReturn {
        g_sink_buffers++;
        return GST_PAD_PROBE_OK;
      },
      nullptr, nullptr);
  gst_object_unref(sinkpad);

  GstBus* bus = gst_element_get_bus(ctx->pipeline);
  gst_bus_add_watch(bus, OnBusMessage, nullptr);
  gst_object_unref(bus);
  return true;
}

// 현재 sink에 협상된 채널수 (0 = 미협상)
int NegotiatedSinkChannels(Ctx* ctx) {
  GstPad* pad = gst_element_get_static_pad(ctx->sink, "sink");
  GstCaps* caps = gst_pad_get_current_caps(pad);
  gst_object_unref(pad);
  if (!caps) return 0;
  int ch = 0;
  gst_structure_get_int(gst_caps_get_structure(caps, 0), "channels", &ch);
  gst_caps_unref(caps);
  return ch;
}

// 플레이어 DoAudioSinkSwap과 동일 패턴: tail src IDLE 프로브 안에서
// 버스 caps 변경 + 전체 패드 matrix 갱신 + sink 통째 교체
void SwitchBus(Ctx* ctx, int new_ch, bool positioned, bool use_asio) {
  struct SwapArgs {
    Ctx* ctx;
    int ch;
    bool positioned;
    bool use_asio;
  };
  auto* args = new SwapArgs{ctx, new_ch, positioned, use_asio};
  GstPad* src = gst_element_get_static_pad(ctx->tail, "src");
  gst_pad_add_probe(
      src, GST_PAD_PROBE_TYPE_IDLE,
      [](GstPad*, GstPadProbeInfo*, gpointer data) -> GstPadProbeReturn {
        auto* a = static_cast<SwapArgs*>(data);
        Ctx* c = a->ctx;

        gst_element_set_locked_state(c->sink, TRUE);
        gst_element_set_state(c->sink, GST_STATE_NULL);
        gst_element_unlink(c->tail, c->sink);
        gst_bin_remove(GST_BIN(c->pipeline), c->sink);

        GstCaps* caps = MakeBusCaps(a->ch, a->positioned);
        g_object_set(c->bus_caps, "caps", caps, nullptr);
        gst_caps_unref(caps);
        // 사인 → 버스 마지막 2채널, 무음 → 전부 뮤트
        SetPadMatrix(c->sine_pad, 2, a->ch, {a->ch - 2, a->ch - 1});
        SetPadMatrix(c->silence_pad, 1, a->ch, {-1});

        if (a->use_asio) {
          c->sink = gst_element_factory_make("asiosink", "asink");
          if (c->sink)
            g_object_set(c->sink, "device-clsid", c->asio_clsid.c_str(), "occupy-all-channels",
                         FALSE, nullptr);
        } else {
          c->sink = gst_element_factory_make("wasapi2sink", "asink");
          if (c->sink && !c->wasapi_device.empty())
            g_object_set(c->sink, "device", c->wasapi_device.c_str(), nullptr);
        }
        gst_bin_add(GST_BIN(c->pipeline), c->sink);
        if (!gst_element_link(c->tail, c->sink)) printf("[swap] relink failed\n");
        gst_element_sync_state_with_parent(c->sink);

        GstPad* sinkpad = gst_element_get_static_pad(c->sink, "sink");
        gst_pad_add_probe(
            sinkpad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad*, GstPadProbeInfo*, gpointer) -> GstPadProbeReturn {
              g_sink_buffers++;
              return GST_PAD_PROBE_OK;
            },
            nullptr, nullptr);
        gst_object_unref(sinkpad);

        delete a;
        return GST_PAD_PROBE_REMOVE;
      },
      args, nullptr);
  gst_object_unref(src);
}

// N ms 동안 메인루프 돌리고, 그 사이 sink 버퍼가 흘렀는지 + 에러 없는지 확인
bool RunPhase(Ctx* ctx, const char* label, int wait_ms, int expect_ch) {
  const guint64 before = g_sink_buffers.load();
  const int err_before = g_error_count.load();
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_timeout_add(wait_ms, [](gpointer data) -> gboolean {
    g_main_loop_quit(static_cast<GMainLoop*>(data));
    return G_SOURCE_REMOVE;
  }, loop);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);

  const guint64 flowed = g_sink_buffers.load() - before;
  const int errors = g_error_count.load() - err_before;
  const int neg_ch = NegotiatedSinkChannels(ctx);
  const bool ok = flowed > 0 && errors == 0 && (expect_ch == 0 || neg_ch == expect_ch);
  printf("[%s] buffers=%llu errors=%d negotiated=%dch (expect %dch) => %s\n", label,
         static_cast<unsigned long long>(flowed), errors, neg_ch, expect_ch,
         ok ? "OK" : "NG");
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  gst_init(&argc, &argv);
  const char* name_filter = argc > 1 ? argv[1] : nullptr;

  Ctx ctx;
  PickDevices(&ctx, name_filter);
  printf("selected wasapi device: %s (%dch cap)\n",
         ctx.wasapi_device.empty() ? "(default)" : ctx.wasapi_device.c_str(), ctx.wasapi_ch);

  if (!BuildPipeline(&ctx)) {
    printf("SPIKE: FAIL (build)\n");
    return 1;
  }
  if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    printf("SPIKE: FAIL (start)\n");
    return 1;
  }

  bool pass = true;
  // 1) 2ch 스테레오 기동
  pass &= RunPhase(&ctx, "phase1 2ch", 2000, 2);

  // 2) 라이브 확장: 2ch → 디바이스 최대(≤8, fallback mask), 사인은 마지막 2채널로
  const int wide = ctx.wasapi_ch >= 2 ? ctx.wasapi_ch : 2;
  SwitchBus(&ctx, wide, /*positioned=*/true, /*use_asio=*/false);
  pass &= RunPhase(&ctx, "phase2 widen", 3000, wide);

  // 3) 라이브 축소: → 2ch
  SwitchBus(&ctx, 2, /*positioned=*/true, /*use_asio=*/false);
  pass &= RunPhase(&ctx, "phase3 shrink", 3000, 2);

  // 4) (옵션) ASIO unpositioned — 버스 = 드라이버 보고 채널수 (asiosink getcaps가
  //    output-channels 미설정 시 전체 채널수로 고정되므로 반드시 일치해야 함)
  if (!ctx.asio_clsid.empty() && ctx.asio_ch > 0) {
    SwitchBus(&ctx, ctx.asio_ch, /*positioned=*/false, /*use_asio=*/true);
    pass &= RunPhase(&ctx, "phase4 asio full-width unpositioned", 4000, ctx.asio_ch);
    // 5) ASIO → 다시 WASAPI 2ch 복귀 (디바이스 종류 왕복)
    SwitchBus(&ctx, 2, /*positioned=*/true, /*use_asio=*/false);
    pass &= RunPhase(&ctx, "phase5 back to wasapi 2ch", 3000, 2);
  } else {
    printf("[phase4] asio device not connected — skipped\n");
  }

  gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
  gst_object_unref(ctx.pipeline);
  printf("SPIKE: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
