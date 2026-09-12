#!/usr/bin/env python3
"""The Overview now-playing card must say what is playing and offer only the
transport the device reported, and its handlers must survive the poll that
replaces the card every second while media is active."""
import pathlib

source = pathlib.Path("web/js/app.js").read_text(encoding="utf-8")
css = pathlib.Path("web/css/app.css").read_text(encoding="utf-8")

# Which audio, not just "media audio": the title, then the station, and only
# then a source-shaped fallback that admits it knows nothing.
assert "radio:'Internet radio'" in source
assert "if(m.title)return{source,title:m.title" in source
assert "if(m.station)return{source,title:m.station,detail:'The station is not sending a track title'}" in source
assert "detail:'This source is not sending track information'" in source
assert "(m.station?'on '+m.station:'Playing now')" in source

# The transport comes from the device, and a control that cannot work is
# disabled and carries the device's own reason.
assert "const t=p.transport||{}" in source
assert "t.stop?{action:'stop',label:'Stop'}:t.play?{action:'play',label:'Play'}:null" in source
assert "${act?'':' disabled'}" in source
assert "${act?'':`<small>${esc(reason)}</small>`}" in source
assert "api('/playback/transport',{method:'POST',body:JSON.stringify({action})})" in source
# No pause button is invented for a device that cannot pause anything.
assert "label:'Pause'" not in source

# The light-ring toggle is the existing visualizer setting, reachable where the
# music is. It reads the live value and writes the same endpoint the LED page
# writes, so the two pages cannot disagree.
assert "toggle('Lights with music',l.visualizer_enabled!==false,'now-playing-visualizer')" in source
assert "api('/led',{method:'PUT',body:JSON.stringify({visualizer_enabled:enabled})})" in source
assert "if(v)v.onchange=()=>setMusicLights(v.checked)" in source

# The card is rebuilt on every poll, so its handlers are bound after every
# render and the poll does not overwrite an interaction still in flight.
assert "function renderNowPlaying(){" in source
assert "bindNowPlaying()" in source
assert "if(!state.busy)renderNowPlaying();" in source
assert "finally{setBusy(false);renderNowPlaying()}" in source
assert "playing.outerHTML=nowPlaying(p,l)" not in source

# The card grew a column for the control; the old four-column grid would put it
# on top of the spectrum.
assert "grid-template-columns:72px minmax(210px,1.5fr) auto minmax(140px,.75fr) auto" in css
assert ".now-playing-transport{" in css

print("now-playing card contract: ok")
