// SPDX-License-Identifier: MIT
// Real patched SDK listener + real C99 sink. No hardware paths.
#include "engine_sink_session.h"
#include "sdk_player_listener.h"
#include "sendspin_engine_fixture.h"
#include <sendspin/client.h>
#include <vector>
using namespace libreecho::sendspin;
using namespace libreecho::sendspin::test;
static uint32_t queued(EngineServer& server) {
    uint32_t n = 0;
    server.with_sink([&](le_audio_sink* s) { le_audio_sink_progress p{};
        CHECK(le_audio_sink_get_progress(s, &p) == LE_AUDIO_SINK_OK, "progress"); n=p.queued_frames; });
    return n;
}
int main() {
    EngineServer server;
    auto path = unique_socket_path("life");
    CHECK(server.start(path), "real sink start");
    EngineSinkSessionConfig cfg; cfg.socket_path=path;
    EngineSinkSession session(cfg);
    ::sendspin::SendspinClientConfig client_cfg;
    ::sendspin::SendspinClient client(client_cfg);
    ::sendspin::PlayerRoleConfig player_cfg;
    player_cfg.audio_formats.push_back({::sendspin::SendspinCodecFormat::PCM,2,48000,16});
    auto& player=client.add_player(player_cfg);
    SdkPlayerListener listener(session,&player);
    std::vector<int16_t> pcm(512*2,123);
    auto offer=[&]() { return listener.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),pcm.size()*2,20); };
    listener.on_stream_start(); CHECK(offer()==pcm.size()*2,"acked whole frames");
    const auto gen=session.generation();
    // Dispatch through the real patched SDK base, not an adapter-only method.
    ::sendspin::PlayerRoleListener& callback=listener;
    callback.on_stream_end(::sendspin::SendspinStreamEndReason::NATURAL);
    CHECK(queued(server)==512,"natural end retains queued tail");
    CHECK(!session.streaming(),"natural end closes writes");
    callback.on_stream_end(::sendspin::SendspinStreamEndReason::ROLE_REMOVED);
    CHECK(queued(server)==0,"removal fences even a FINISHed tail");
    listener.on_stream_start(); CHECK(offer()==pcm.size()*2,"new stream accepts");
    CHECK(session.generation()>gen,"successor generation");
    const auto before=session.generation();
    CHECK(callback.on_stream_boundary(1)==512,"boundary retires accepted unreported ledger");
    CHECK(queued(server)==0,"boundary drops old unrendered PCM");
    CHECK(session.streaming() && session.generation()>before,"seek rearms without stream/start");
    CHECK(callback.on_stream_boundary(1)==0,"duplicate token does not reset again");
    CHECK(offer()==pcm.size()*2,"postseek accepts");
    callback.on_stream_clear();
    CHECK(queued(server)==512,"late informational clear cannot drop postseek bytes");
    callback.on_stream_end(::sendspin::SendspinStreamEndReason::DISCONNECTED);
    CHECK(queued(server)==0 && offer()==0,"disconnect drops queued tail and disarms");
    listener.on_stream_start(); CHECK(offer()==pcm.size()*2,"restart accepts before delayed FINISH");
    server.pause();
    callback.on_stream_end(::sendspin::SendspinStreamEndReason::NATURAL);
    CHECK(!session.streaming(),"natural end fences local writes before waiting for FINISH_ACK");
    CHECK(offer()==0,"natural end stops writes even when FINISH_ACK misses deadline");
    server.resume();
    bool finished=false;
    for(int i=0;i<30 && !finished;++i) {
        listener.pump_feedback();
        server.with_sink([&](le_audio_sink* s){le_audio_sink_progress p{};
            le_audio_sink_get_progress(s,&p); finished=p.finished;});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(finished,"feedback loop retries the bounded natural FINISH");
    listener.on_stream_start(); CHECK(offer()==pcm.size()*2,"feedback-prefix start");
    server.pause(); CHECK(offer()==0,"second block awaits late CREDIT");
    server.resume(); server.pump(4);
    server.with_sink([&](le_audio_sink* s){
        int16_t rendered[2048]; uint64_t first=0;
        const auto frames=le_audio_sink_render(s,rendered,1024,&first);
        CHECK(frames==1024,"engine accepted both blocks");
        le_audio_sink_commit(s,session.epoch(),session.generation(),frames);
        le_audio_sink_note_playhead(s,session.epoch(),session.generation(),1024);
        le_audio_sink_note_timing(s,session.epoch(),session.generation(),LE_AUDIO_SINK_PROGRESS_TIMING_VALID,4000000);
    });
    CHECK(listener.pump_feedback() && listener.reported_frames()==512,
          "feedback reports only acknowledged played prefix while late CREDIT is held");
    CHECK(offer()==pcm.size()*2,"identical offer receives held credit");
    CHECK(listener.pump_feedback() && listener.reported_frames()==1024,"held tail reported after SDK acknowledges it");
    session.disconnect(); server.stop();
    if(g_failures) return 1;
    std::puts("test_sendspin_sdk_lifecycle: all checks passed"); return 0;
}
