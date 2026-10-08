'use strict';
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('web/js/app.js','utf8');
/* Execute the real presentation functions, without startup network requests. */
const start=source.indexOf('function playbackSource('),end=source.indexOf('/*\n * The transport',start);
assert(start>=0&&end>start);
const c={};vm.createContext(c);vm.runInContext(source.slice(start,end),c);
const playing=(source,title)=>({state:'playing',source,metadata:{title}});
for(const title of ['White Noise','Pink Noise','Brown Noise','Heartbeat']){
 const copy=c.nowPlayingCopy(playing('noise',title));
 assert.strictEqual(copy.title,title);assert.strictEqual(copy.source,'Sleep audio');assert.strictEqual(copy.detail,'Playing now');
}
assert.strictEqual(c.nowPlayingCopy({state:'idle',source:null,metadata:{}}).title,'Nothing playing');
assert.strictEqual(c.nowPlayingCopy(playing('media',null)).title,'Media audio');
const media=playing('airplay2','Real track');media.metadata.artist='Artist';media.metadata.album='Album';
assert.strictEqual(c.nowPlayingCopy(media).title,'Real track');assert.strictEqual(c.nowPlayingCopy(media).detail,'Artist · Album');
assert.strictEqual(c.nowPlayingCopy({state:'announcing',source:'noise',metadata:{title:'Brown Noise'}}).title,'Announcement in progress');
assert.strictEqual(c.nowPlayingCopy({state:'alarm',source:'noise',metadata:{title:'Brown Noise'}}).title,'Alarm active');
console.log('noise now-playing presentation: labels, stop, source switch and priority: ok');
