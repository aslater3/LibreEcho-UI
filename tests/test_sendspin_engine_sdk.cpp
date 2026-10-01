// SPDX-License-Identifier: MIT
// Real patched SDK sync thread -> session -> production run_engine in a
// separate process. TinyALSA is fake; no DAC or multiroom hardware claim.
#include "lifecycle_test_fixtures.h"
#include "engine_sink_session.h"
#include "sdk_player_listener.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
using namespace sendspin;
using libreecho::sendspin::EngineSinkSession;
using libreecho::sendspin::EngineSinkSessionConfig;
using libreecho::sendspin::SdkPlayerListener;
using libreecho::sendspin::SinkResult;

static bool pump(SendspinClient& client, SdkPlayerListener& listener,
                 const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now()<deadline) {
        listener.pump_feedback();
        if(predicate()) return true;
        pump_for(client,5);
    }
    return predicate();
}
class PcmServer : public FakeEncryptedServer {
public:
    using FakeEncryptedServer::FakeEncryptedServer;
    bool send_pcm(int64_t timestamp,size_t frames,uint64_t first) {
        std::string body(4,'\0'); // PCM send-ahead prefix
        for(size_t i=0;i<frames;++i) {
            // Keep the ramp positive and below saturation under the engine DSP.
            const uint16_t left=1000+static_cast<uint16_t>((first+i)%1000);
            const uint16_t right=2000+static_cast<uint16_t>((first+i)%1000);
            for(uint16_t sample:{left,right}) {
                body.push_back(static_cast<char>(sample&255));
                body.push_back(static_cast<char>(sample>>8));
            }
        }
        return send_binary(4,timestamp,body);
    }
};
// Test-only observation of what the real listener actually acknowledged.
// Generated SDK priming silence alone cannot satisfy playback acceptance.
class ObservedListener : public SdkPlayerListener {
public:
    using SdkPlayerListener::SdkPlayerListener;
    std::atomic<uint64_t> nonzero_bytes{0};
    size_t on_audio_write(uint8_t* data,size_t length,uint32_t timeout_ms) override {
        const auto accepted=SdkPlayerListener::on_audio_write(data,length,timeout_ms);
        uint64_t count=0;
        for(size_t i=0;i<accepted;++i) if(data[i]!=0) ++count;
        nonzero_bytes.fetch_add(count);
        return accepted;
    }
};
static int failures=0;
#define REQUIRE(condition,message) do { if(!(condition)) { \
    std::fprintf(stderr,"FAIL: %s\n",message); ++failures; } } while(0)

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    EngineSinkSessionConfig sink_cfg; sink_cfg.socket_path=argv[1];
    sink_cfg.control_timeout_ms=50; sink_cfg.max_write_timeout_ms=20;
    EngineSinkSession session(sink_cfg);
    // The fixture uses only a loopback encrypted transport and ephemeral keys.
    constexpr uint16_t port=19321;
    SendspinClientConfig config; config.name="LibreEcho engine host fixture";
    config.server_port=port; config.time_burst_interval_ms=100;
    PairedClientBundle bundle(std::move(config));
    auto& player=bundle.client().add_player(make_pcm_player_config());
    ObservedListener listener(session,&player);
    player.set_listener(&listener);
    REQUIRE(bundle.start(),"SDK client start");
    FakeEncryptedServerOptions options; options.answer_time=true;
    options.first_roles_json=R"(["player@v1"])";
    PcmServer server(server_url(port),std::string(NOISE_SUITE_CHACHAPOLY),
        bundle.peer.server_identity,bundle.peer.record.psk_id,bundle.peer.psk,options);
    auto& client=bundle.client();
    REQUIRE(pump(client,listener,[&]{return client.is_connected()&&client.is_time_synced();},4000),
            "encrypted loopback connection and time sync");
    REQUIRE(server.send_app_json(stream_start_pcm_json()),"SDK stream/start");
    REQUIRE(pump(client,listener,[&]{return session.streaming();},3000),"real engine generation opened");
    int64_t timestamp=platform_time_us()+100000;
    uint64_t first=0;
    int64_t minimum_send_ahead_us=INT64_MAX;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while((session.accepted_frames()<8192 || listener.nonzero_bytes.load()==0) &&
          std::chrono::steady_clock::now()<deadline) {
        minimum_send_ahead_us=std::min(minimum_send_ahead_us,timestamp-platform_time_us());
        REQUIRE(server.send_pcm(timestamp,960,first),"encrypted PCM message");
        first+=960; timestamp+=20000;
        listener.pump_feedback(); pump_for(client,20);
    }
    std::printf("PCM_SEND minimum_ahead_us=%lld sent_frames=%llu nonzero_bytes=%llu\n",
        static_cast<long long>(minimum_send_ahead_us),
        static_cast<unsigned long long>(first),
        static_cast<unsigned long long>(listener.nonzero_bytes.load()));
    REQUIRE(session.accepted_frames()>=8192,"SDK decoded PCM credited by real engine");
    REQUIRE(listener.nonzero_bytes.load()>0,"decoded server PCM, not just SDK priming silence, was acknowledged");
    libreecho::sendspin::EngineSinkTiming observed{};
    const auto poll_result=session.poll(&observed);
    std::printf("ENGINE_PROGRESS result=%d valid=%d submitted=%llu played=%llu queued=%u finish=%llu acknowledged=%llu reports=%llu\n",
        static_cast<int>(poll_result),static_cast<int>(observed.valid),
        static_cast<unsigned long long>(observed.submitted_frames),
        static_cast<unsigned long long>(observed.played_frames),observed.queued_frames,
        static_cast<unsigned long long>(observed.finish_us),
        static_cast<unsigned long long>(session.accepted_frames()),
        static_cast<unsigned long long>(listener.report_count()));
    REQUIRE(pump(client,listener,[&]{return listener.reported_frames()>0;},3000),
            "physical-cursor feedback from real engine timing ledger");
    REQUIRE(server.send_app_json(R"({"type":"stream/end"})"),"SDK natural stream/end");
    REQUIRE(pump(client,listener,[&]{return listener.stream_ends()!=0;},3000),"typed natural end delivered");
    REQUIRE(!session.streaming(),"natural end fenced further SDK writes");
    bool completed=pump(client,listener,[&]{
        libreecho::sendspin::EngineSinkTiming timing{};
        return session.poll(&timing)==SinkResult::Ok && timing.valid &&
            timing.played_frames==session.accepted_frames() && timing.queued_frames==0 &&
            listener.reported_frames()==session.accepted_frames();
    },4000);
    REQUIRE(completed,"entire accepted natural tail reaches physical cursor and feedback");
    const uint64_t accepted=session.accepted_frames();
    std::printf("SDK_ENGINE_RESULT accepted=%llu reported=%llu completed=%d\n",
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(listener.reported_frames()),static_cast<int>(completed));
    client.stop(); session.disconnect();
    return failures?1:0;
}
