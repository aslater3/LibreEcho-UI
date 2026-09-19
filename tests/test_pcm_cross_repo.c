#define _GNU_SOURCE
#include "live_audio_out.h"
#include "pcm_stream_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"paired PCM line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct le_pcm_server engine;
static struct le_live_audio_out speech;
static char root[] = "/tmp/le-pcm-paired-XXXXXX";
static uint64_t hardware;
static void render(void)
{
    le_pcm_server_service(&engine);
    if (le_pcm_server_ready(&engine, 2048)) {
        le_pcm_server_submit(&engine, hardware, 2048);
        hardware += 2048;
        le_pcm_server_progress(&engine, hardware);
    }
}
static void exact_tail(size_t count, unsigned int rate)
{
    int16_t input[LE_LIVE_AUDIO_SAMPLES];
    unsigned int step;
    int drained = 0;
    uint64_t expected = count * 48000U / rate;
    size_t i;
    for (i=0;i<count;++i) input[i]=1234;
    CHECK(le_live_audio_out_write(&speech,input,count,rate)==0);
    for(step=0;step<50 && !drained;++step) {
        render();
        drained=le_live_audio_out_drained(&speech);
    }
    CHECK(drained && speech.progress.state==LE_PCM_DRAINED);
    CHECK(speech.progress.accepted_frames==expected && speech.progress.played_frames==expected);
    CHECK(le_pcm_server_focus(&engine)); /* Conversation survives the utterance. */
}
int main(void)
{
    char bus[128];
    int cue,music,step;
    int16_t music_pcm[4096],one[2]={900,900},input[1280];
    struct le_pcm_progress cue_progress={0},music_progress={0};
    CHECK(mkdtemp(root));CHECK(le_pcm_server_open(&engine,root)==0);
    CHECK(snprintf(bus,sizeof(bus),"%s/system.pcm",root)>0);
    CHECK(unsetenv("LE_LIVE_ALLOW_LEGACY_TEST_SINK")==0);
    le_live_audio_out_init(&speech,bus,48000);
    CHECK(le_live_audio_out_focus(&speech,1)==0);
    le_pcm_server_service(&engine);CHECK(le_pcm_server_focus(&engine));
    exact_tail(1,24000);exact_tail(1024,24000);exact_tail(1025,24000);
    exact_tail(1280,22050);exact_tail(17,8000);
    for(step=0;step<4096;++step) music_pcm[step]=100;
    for(step=0;step<1280;++step) input[step]=400;
    music=le_pcm_open(bus,0,0,0);cue=le_pcm_open(bus,1,0,0);
    CHECK(music>=0 && cue>=0);
    CHECK(le_pcm_write(music,music_pcm,sizeof(music_pcm))==4096);
    CHECK(le_pcm_write(music,music_pcm+2048,4096)==4096);
    CHECK(le_pcm_write(cue,one,sizeof(one))==sizeof(one));CHECK(le_pcm_finish(cue)==0);
    CHECK(le_live_audio_out_write(&speech,input,1280,24000)==0);
    le_pcm_server_service(&engine);
    le_live_audio_out_cancel(&speech);
    le_pcm_server_service(&engine);
    CHECK(le_pcm_server_mix(&engine,0,2048,32768)==1000);
    CHECK(le_pcm_server_mix(&engine,1,2048,32768)==100);
    render();CHECK(le_pcm_progress_read(cue,&cue_progress)>=0);
    CHECK(cue_progress.state==LE_PCM_DRAINED && cue_progress.played_frames==1);
    CHECK(le_pcm_progress_read(music,&music_progress)>=0);
    CHECK(music_progress.state==LE_PCM_ACCEPTING && music_progress.played_frames==2048);
    close(cue);close(music);le_pcm_server_service(&engine);
    exact_tail(19,24000); /* New reply after cancellation. */
    le_live_audio_out_close(&speech);le_pcm_server_service(&engine);
    CHECK(!le_pcm_server_focus(&engine));
    le_pcm_server_close(&engine);CHECK(rmdir(root)==0);
    puts("paired UI/Platform PCM: exact tails, scoped cancellation, cues, music and focus PASS");
    return 0;
}
