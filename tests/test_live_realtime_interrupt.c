#define _POSIX_C_SOURCE 200809L
#define LE_TLS_AVAILABLE 0
/* Exercise the real translator and masked outbound wire, not a mock transport. */
#include "../src/adapter/live_transport_realtime.c"
#include <assert.h>
static unsigned char sent[32768];
static size_t sent_used;
static long capture_send(void *unused,const void *bytes,size_t count)
{
    (void)unused;assert(count<=sizeof(sent)-sent_used);
    memcpy(sent+sent_used,bytes,count);sent_used+=count;return (long)count;
}
static long quiet_recv(void *unused,void *bytes,size_t count)
{ (void)unused;(void)bytes;(void)count;return -2; }
static size_t wire_text(char *output,size_t size)
{
    size_t at=0,used=0;
    while(at<sent_used) {
        unsigned int n=sent[at+1]&127U,header=2,i;
        if(n==126U) { n=(unsigned int)sent[at+2]*256U+sent[at+3];header=4; }
        assert(n<65536U && at+header+4U+n<=sent_used && used+n+2<size);
        for(i=0;i<n;++i) output[used++]=(char)(sent[at+header+4U+i]^sent[at+header+i%4U]);
        output[used++]='\n';at+=header+4U+n;
    }
    output[used]='\0';return used;
}
int main(void)
{
    struct le_live_transport transport={0};
    struct le_ws_stream stream={NULL,quiet_recv,capture_send};
    struct le_live_event event={0};
    int16_t audio[2400]={0};
    char encoded[6500],message[7200],wire[32768];
    memset(&state,0,sizeof(state));
    state.ws.stream=&stream;state.ws.connected=1;state.ws.urandom=open("/dev/urandom",O_RDONLY);
    assert(state.ws.urandom>=0);
    assert(translate("{\"type\":\"response.created\",\"response\":{\"id\":\"r1\"}}",&event)==0);
    assert(le_b64_encode(audio,sizeof(audio),encoded,sizeof(encoded))>0);
    snprintf(message,sizeof(message),"{\"type\":\"response.output_audio.delta\",\"response_id\":\"r1\",\"item_id\":\"a1\",\"content_index\":0,\"delta\":\"%s\"}",encoded);
    assert(translate(message,&event)==1 && event.kind==LE_LIVE_EVENT_AUDIO);
    transport.output_played_ms=30;
    assert(ws_interrupt(&transport)==0);
    assert(wire_text(wire,sizeof(wire))>0);
    assert(strstr(wire,"\"type\":\"response.cancel\",\"response_id\":\"r1\""));
    assert(strstr(wire,"\"item_id\":\"a1\",\"content_index\":0,\"audio_end_ms\":30"));
    assert(!state.audio_pending_count && state.suppress_audio);
    assert(translate(message,&event)==0); /* late audio cannot reopen the old generation */
    assert(translate("{\"type\":\"response.done\",\"response\":{\"id\":\"r1\"}}",&event)==0);
    assert(!state.output_done_pending);
    assert(translate("{\"type\":\"response.function_call_arguments.done\",\"response_id\":\"r1\",\"call_id\":\"old\",\"name\":\"timer_set\",\"arguments\":\"{\\\"seconds\\\":5}\"}",&event)==0);
    assert(translate("{\"type\":\"response.created\",\"response\":{\"id\":\"r2\"}}",&event)==0);
    snprintf(message,sizeof(message),"{\"type\":\"response.output_audio.delta\",\"response_id\":\"r2\",\"item_id\":\"a2\",\"delta\":\"%s\"}",encoded);
    assert(translate(message,&event)==1 && !state.suppress_audio);
    assert(ws_poll(&transport,&event,0)==1 && event.kind==LE_LIVE_EVENT_AUDIO);
    assert(translate("{\"type\":\"response.done\",\"response\":{\"id\":\"r2\"}}",&event)==0);
    assert(ws_poll(&transport,&event,0)==1 && event.kind==LE_LIVE_EVENT_OUTPUT_DONE);
    /* Tool follow-up is queued only after the active response's terminal event. */
    assert(translate("{\"type\":\"response.created\",\"response\":{\"id\":\"r3\"}}",&event)==0);
    sent_used=0;
    assert(ws_complete_delegation(&transport,"fn:call3","{}") == 0);
    wire_text(wire,sizeof(wire));assert(!strstr(wire,"response.create"));
    assert(translate("{\"type\":\"response.done\",\"response\":{\"id\":\"r3\"}}",&event)==0);
    wire_text(wire,sizeof(wire));assert(strstr(wire,"response.create"));
    le_ws_close(&state.ws);
    puts("Realtime interruption: cancel, played-position truncation, stale-event isolation and continuation PASS");
    return 0;
}
