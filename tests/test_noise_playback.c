/* Exercise the production playback response with deterministic backend state.
 * Function sections discard unrelated API routes; no device/service access. */
#include "../src/api.c"
#include <assert.h>
static struct le_playback_state current;
static struct le_audio_state audio;
static struct le_radio_status radio;
static int audio_rc;
const char *le_result_code(int rc){(void)rc;return "io_error";}
int le_get_playback_state(struct le_backend*b,struct le_playback_state*p){(void)b;*p=current;return LE_OK;}
int le_get_audio_state(struct le_backend*b,struct le_audio_state*a){(void)b;*a=audio;return audio_rc;}
int le_radio_playing(struct le_backend*b,struct le_radio_status*r){(void)b;*r=radio;return LE_OK;}
static struct api_context context;
static struct api_response response;
static void read_playback(void){memset(&response,0,sizeof(response));playback_json(&context,&response);assert(response.status==200);assert(json_valid_object(response.body,strlen(response.body)));}
static void title(const char*expected){char actual[256];read_playback();assert(json_get_string(response.body,"title",actual,sizeof(actual))>0);assert(!strcmp(actual,expected));}
static void no_noise(void){read_playback();assert(!strstr(response.body,"\"source\":\"noise\""));assert(!strstr(response.body,"Noise"));assert(!strstr(response.body,"Heartbeat"));}
int main(void){
 const char*sources[]={"white","pink","brown","heartbeat"};
 const char*titles[]={"White Noise","Pink Noise","Brown Noise","Heartbeat"};size_t i;
 strcpy(current.state,"playing");strcpy(current.source,"media");current.media_active=1;audio.noise_active=1;
 /* Sequential source changes must not cache the preceding colour. */
 for(i=0;i<4;i++){snprintf(audio.noise_source,sizeof(audio.noise_source),"%s",sources[i]);title(titles[i]);assert(strstr(response.body,"\"source\":\"noise\""));assert(strstr(response.body,"\"available\":true"));assert(strstr(response.body,"\"artist\":null,\"album\":null,\"station\":null"));}
 audio.noise_source[0]=0;strcpy(audio.noise_colour,"pink");title("Pink Noise");
 strcpy(audio.noise_source,"heartbeat");title("Heartbeat"); /* never legacy pink */
 strcpy(audio.noise_source,"unknown");no_noise();
 strcpy(audio.noise_source,"brown");audio.noise_active=0;no_noise(); /* stop/timer expiry */
 audio.noise_active=1;audio_rc=LE_NOT_SUPPORTED;no_noise();audio_rc=LE_IO;no_noise();audio_rc=LE_OK;
 memset(&current,0,sizeof(current));strcpy(current.state,"idle");title("Brown Noise");assert(strstr(response.body,"\"state\":\"playing\""));assert(strstr(response.body,"\"media\":true"));
 audio.noise_active=0;no_noise();assert(strstr(response.body,"\"state\":\"idle\""));assert(strstr(response.body,"\"title\":null"));audio.noise_active=1;
 strcpy(current.state,"playing");
 for(i=0;i<3;i++){const char*names[]={"airplay2","bluetooth","spotify"};strcpy(current.source,names[i]);no_noise();}
 strcpy(current.source,"airplay2");strcpy(current.title,"Real track");strcpy(current.artist,"Real artist");strcpy(current.album,"Real album");current.metadata_available=1;title("Real track");assert(strstr(response.body,"Real artist"));assert(strstr(response.body,"Real album"));
 strcpy(current.source,"media");title("Real track");
 memset(&current,0,sizeof(current));strcpy(current.state,"playing");strcpy(current.source,"media");strcpy(current.artist,"Artist only");no_noise();current.artist[0]=0;current.metadata_available=1;no_noise();current.metadata_available=0;
 for(i=0;i<3;i++){const char*states[]={"announcing","alarm","system"};strcpy(current.state,states[i]);no_noise();}
 strcpy(current.state,"playing");radio.playing=1;strcpy(radio.title,"Radio track");strcpy(radio.station,"Radio station");title("Radio track");assert(strstr(response.body,"\"source\":\"radio\""));radio.title[0]=0;read_playback();assert(strstr(response.body,"\"title\":null"));assert(strstr(response.body,"Radio station"));radio.playing=0;
 /* A remembered radio must not offer Play over an active sleep generator. */
 context.radio_count=1;strcpy(context.radio_last_word,"station");strcpy(context.radio[0].word,"station");context.radio[0].enabled=1;title("Brown Noise");assert(strstr(response.body,"\"play\":false,\"pause\":false,\"stop\":false"));assert(strstr(response.body,"Audio page"));
 puts("noise playback: colours, legacy/source state, stop/expiry, priority, metadata and transport: ok");return 0;
}
