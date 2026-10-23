/*
 * ESP8266 WiFi Oscilloscope  -  Stage 2 (real ADC on A0)
 * ---------------------------------------------------------------
 * Board   : NodeMCU 1.0 (ESP-12E Module)
 * Mode    : WiFi AP   SSID: ESP-Scope   PASS: 12345678
 * URL     : http://192.168.4.1   (opens automatically via captive portal)
 * Library : NONE needed (core ESP8266WiFi + ESP8266WebServer only, core 2.5.0+ OK)
 *
 * Stage 1 content
 *   - AP + HTTP server (page stored in flash) + HTTP polling data link
 *   - Professional black-background single channel (CH1) front-end
 *   - Data source DEMO (simulated waveform) or A0 (real ADC, 0~3.3V ONLY)
 *   - Run / Stop / Single / Auto Scale, V/div, Time/div, positions,
 *     edge trigger (Auto / Normal), AC/DC, probe x1/x10, measurements
 *
 * GET /d        -> [uint32 fs][uint32 meta: 0 full frame / 1 roll chunk] + n x [uint16 raw ADC]
 *                  (HTTP 204 when nothing new is ready)
 * GET /c?c=<cmd> -> run | stop | a:<fs>,<len>,<roll> | src:demo|adc | wave:0|1|2 |
 *                   freq:<Hz> | bench
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>          // captive portal (part of the ESP8266 core)

// ------------------------- user settings -------------------------
const char* AP_SSID = "ESP-Scope";
const char* AP_PASS = "12345678";          // at least 8 characters
const uint8_t AP_CHANNEL = 6;

const uint16_t N_SAMPLES = 1024;           // samples per frame (memory depth)
const uint32_t FS_MAX_DEMO = 20000;
uint32_t fsMaxAdc = 8000;                  // replaced by the measured value after "ADC TEST"

// ------------------------- globals -------------------------------
ESP8266WebServer server(80);
DNSServer dnsServer;

uint16_t sampleBuf[N_SAMPLES];
uint8_t  txBuf[8 + 2 * N_SAMPLES] __attribute__((aligned(4)));

bool     running = true;
bool     demoMode = true;
uint32_t fsReq = 16000;
uint8_t  demoWave = 0;                     // 0 sine, 1 square, 2 triangle
float    demoFreq = 1000.0f;

uint32_t lastFrameMs = 0;
bool     frameReady = false;       // a complete frame waits in txBuf
uint32_t lastPollMs = 0;           // last time the browser polled /d

// ADC acquisition state machine (non-blocking so WiFi keeps running)
bool     acqActive = false;
uint16_t acqIdx = 0;
uint32_t acqNext = 0, acqStart = 0, acqPeriod = 0;

// ------------------------- web page (flash) ----------------------
const char INDEX_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP-Scope</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:#000;color:#c8c8c8;
  font-family:Consolas,Menlo,"Courier New",monospace;font-size:13px;
  -webkit-user-select:none;user-select:none}
#app{display:grid;grid-template-rows:36px minmax(0,1fr) auto;height:100vh;height:100dvh}
header{display:flex;align-items:center;gap:14px;padding:0 12px;background:#111;
  border-bottom:1px solid #2a2a2a;white-space:nowrap;overflow:hidden}
.brand{font-weight:bold;letter-spacing:2px;color:#ddd}
.brand b{color:#ffd400}
.badge{padding:2px 8px;border-radius:3px;font-weight:bold;border:1px solid #444;min-width:54px;text-align:center}
.badge.run{background:#0b3d0b;border-color:#1f9d1f;color:#4dff4d}
.badge.stop{background:#4a0b0b;border-color:#c02020;color:#ff5a5a}
.badge.single{background:#4a3a00;border-color:#d0a000;color:#ffd400}
.badge.trg{color:#ffd400;border-color:#554a00;background:#171400}
.hi{color:#e8e8e8}
.hi.y{color:#ffd400}
.sp{flex:1}
.led{width:10px;height:10px;border-radius:50%;background:#a00;display:inline-block}
.led.on{background:#2f2;box-shadow:0 0 6px #2f2}
main{display:grid;grid-template-columns:minmax(0,1fr) 264px;min-height:0}
#scopeWrap{position:relative;min-height:0;background:#000}
#scope{position:absolute;left:0;top:0;display:block;touch-action:none}
aside{background:#0d0d0d;border-left:1px solid #2a2a2a;overflow-y:auto;padding:6px 10px 12px}
aside h3{margin:12px 0 4px;font-size:11px;letter-spacing:2px;color:#888;
  border-bottom:1px solid #262626;padding-bottom:3px}
aside h3.ch{color:#ffd400}
.row{display:flex;gap:6px;margin:6px 0}
button,select{flex:1;background:#1a1a1a;color:#ddd;border:1px solid #3a3a3a;border-radius:3px;
  padding:7px 4px;font:inherit;cursor:pointer;min-width:0}
button:hover,select:hover{border-color:#777}
button:active{background:#333}
button.on{background:#3a3000;border-color:#ffd400;color:#ffd400}
button.off{color:#777}
#bRun.run{background:#0b3d0b;border-color:#1f9d1f;color:#4dff4d}
#bRun.stop{background:#4a0b0b;border-color:#c02020;color:#ff5a5a}
#bSingle.armed{background:#4a3a00;border-color:#ffd400;color:#ffd400}
.ctl{display:flex;align-items:center;gap:6px;margin:6px 0}
.ctl label,.sl label{width:62px;font-size:11px;color:#888}
.ctl button{flex:0 0 36px;font-weight:bold}
.ctl output{flex:1;text-align:center;background:#000;border:1px solid #2e2e2e;padding:5px 2px;color:#ffd400}
.sl{display:flex;align-items:center;gap:6px;margin:8px 0}
.sl input{flex:1;min-width:0;accent-color:#ffd400}
.sl input[type=number]{background:#000;color:#ffd400;border:1px solid #3a3a3a;border-radius:3px;padding:5px;font:inherit}
#adcInfo{font-size:11px;color:#9a9a9a;margin:4px 0;min-height:15px}
button.dis,select.dis{opacity:.35;pointer-events:none}
footer{display:flex;flex-wrap:wrap;background:#111;border-top:1px solid #2a2a2a}
.m{flex:1 1 90px;padding:4px 10px;border-right:1px solid #222;min-width:90px}
.m i{display:block;font-style:normal;font-size:10px;color:#ffd400;letter-spacing:1px}
.m b{font-size:15px;color:#f0f0f0;font-weight:normal}
@media (max-width:820px){
  #app{height:auto;min-height:100vh;grid-template-rows:auto auto auto}
  header{height:auto;min-height:36px;flex-wrap:wrap;gap:6px 12px;padding:6px 10px;white-space:normal}
  main{grid-template-columns:1fr;grid-template-rows:auto auto}
  #scopeWrap{height:58vh;min-height:280px}
  aside{border-left:0;border-top:1px solid #2a2a2a}
}
</style>
</head>
<body>
<div id="app">
<header>
  <span class="brand">ESP<b>SCOPE</b></span>
  <span id="stRun" class="badge stop">STOP</span>
  <span id="stTrig" class="badge trg">--</span>
  <span id="stClip" class="badge stop" style="display:none">OVER</span>
  <span id="hCH" class="hi y">CH1</span>
  <span id="hH" class="hi">H</span>
  <span id="hT" class="hi">T</span>
  <span id="hFs" class="hi">Fs --</span>
  <span id="hMem" class="hi">Mem --</span>
  <span class="sp"></span>
  <span id="fpsv" class="hi">0 fps</span>
  <span id="led" class="led"></span><span id="conn">OFFLINE</span>
</header>
<main>
  <div id="scopeWrap"><canvas id="scope"></canvas></div>
  <aside>
    <h3>RUN CONTROL</h3>
    <div class="row"><button id="bRun" class="run">RUN / STOP</button><button id="bSingle">SINGLE</button></div>
    <div class="row"><button id="bAuto">AUTO SCALE</button><button id="bForce">FORCE</button></div>

    <h3 class="ch">VERTICAL &middot; CH1</h3>
    <div class="row"><button id="bCh" class="on">CH1 ON</button><button id="bCpl">DC</button><button id="bProbe">1X</button></div>
    <div class="ctl"><label>SCALE</label><button id="vDn">&minus;</button><output id="vVal">1 V</output><button id="vUp">+</button></div>
    <div class="sl"><label>POSITION</label><input id="vPos" type="range" min="-12" max="12" step="0.05" value="-3"></div>

    <h3>HORIZONTAL</h3>
    <div class="ctl"><label>TIME/DIV</label><button id="tDn">&minus;</button><output id="tVal">500 &micro;s</output><button id="tUp">+</button></div>
    <div class="sl"><label>POSITION</label><input id="hPos" type="range" min="-4" max="4" step="0.05" value="0"></div>
    <div class="row"><button id="bInterp" class="on">INTERP: SMOOTH</button></div>

    <h3>TRIGGER</h3>
    <div class="row"><button id="bTAuto" class="on">AUTO</button><button id="bTNorm">NORMAL</button><button id="bSlope">&#8593; RISE</button></div>
    <div class="sl"><label>LEVEL</label><input id="trLvl" type="range" min="-5" max="5" step="0.01" value="1.65"></div>

    <h3>ACQUIRE</h3>
    <div class="row"><button id="bDemo" class="on">DEMO</button><button id="bAdc">A0 (ADC)</button></div>
    <div class="row">
      <select id="selWave"><option value="0">Sine</option><option value="1">Square</option><option value="2">Triangle</option></select>
      <select id="selFreq">
        <option value="50">50 Hz</option><option value="100">100 Hz</option><option value="200">200 Hz</option>
        <option value="500">500 Hz</option><option value="1000" selected>1 kHz</option>
        <option value="2000">2 kHz</option><option value="5000">5 kHz</option>
      </select>
    </div>
    <div class="sl"><label>FULL SCALE</label><input id="vref" type="number" min="2" max="4" step="0.01" value="3.2"><span>V</span></div>
    <div class="row"><button id="bBench">ADC TEST</button></div>
    <div id="adcInfo">ADC max -- | Heap --</div>
    <div style="font-size:11px;color:#777;margin-top:10px;line-height:1.5">
      Keys: Space = Run/Stop, S = Single, A = Auto Scale.<br>
      Double-click a slider to reset it.<br>
      A0 input range: 0 ~ 3.3 V only (NodeMCU full scale is about 3.2 V; calibrate FULL SCALE with a multimeter).<br>
      Drag the left edge of the screen = POSITION, right edge = trigger LEVEL.
    </div>
  </aside>
</main>
<footer>
  <div class="m"><i>Vpp</i><b id="mVpp">--</b></div>
  <div class="m"><i>Vmax</i><b id="mMax">--</b></div>
  <div class="m"><i>Vmin</i><b id="mMin">--</b></div>
  <div class="m"><i>Vavg</i><b id="mAvg">--</b></div>
  <div class="m"><i>Vrms</i><b id="mRms">--</b></div>
  <div class="m"><i>Freq</i><b id="mFrq">--</b></div>
  <div class="m"><i>Period</i><b id="mPer">--</b></div>
</footer>
</div>

<script>
(function(){
'use strict';
var VREF=3.3, ADC_MAX=1023, FS_MAX={demo:20000,adc:8000}, PTS=80, PTS_ROLL=50, PTS_TRIG=50, vrefAdc=3.2, ROLL_MIN_T=0.02,
  rollBuf=[], rollFs=0, rollCount=0, rollTotal=0, rollPos=0, rollStartIdx=0, rollT=0;
var VDIVS=[0.01,0.02,0.05,0.1,0.2,0.5,1,2,5];
var TDIVS=[100e-6,200e-6,500e-6,1e-3,2e-3,5e-3,10e-3,20e-3,50e-3,100e-3,200e-3];
var st={run:'run',trigMode:'auto',slope:1,trigV:1.65,vIdx:6,tIdx:2,pos:-3,hpos:0,
  cpl:'DC',probe:1,ch:true,src:'demo',trigState:'AUTO',memN:1024,smooth:true,force:false,skip:0,singleDone:false};
var last=null,disp=null,meas=null,ws=null,connected=false,fpsCnt=0;
var $=function(id){return document.getElementById(id);};
var cv=$('scope'),cx=cv.getContext('2d');
var geom=null,drag=null;

/* ---------- formatting ---------- */
function eng(x,unit,d){
  if(!isFinite(x)) return '--';
  if(x===0) return '0 '+unit;
  d=d||3;
  var P=['p','n','\u00b5','m','','k','M'],i=4,m=x;
  while(Math.abs(m)>=1000&&i<6){m/=1000;i++;}
  while(Math.abs(m)<1&&i>0){m*=1000;i--;}
  if(Math.abs(Number(m.toPrecision(d)))>=1000&&i<6){m/=1000;i++;}
  return m.toPrecision(d)+' '+P[i]+unit;
}
function expectedFs(){
  var t=TDIVS[st.tIdx];
  return Math.max(10,Math.min(FS_MAX[st.src],Math.round((isRoll()?PTS_ROLL:(trigWide()?PTS_TRIG:PTS))/t)));
}
function minT(){
  var fm=FS_MAX[st.src],i;
  for(i=0;i<TDIVS.length;i++){if(10*TDIVS[i]*fm>=20) return i;}
  return TDIVS.length-1;
}
function clampT(){var m=minT();if(st.tIdx<m)st.tIdx=m;}
function isRoll(){return TDIVS[st.tIdx]>=ROLL_MIN_T-1e-9;}
/* Normal / Single must catch an edge inside ONE capture: use the whole buffer
   and fewer points per screen, so the trigger search window is much wider */
function trigWide(){return !isRoll()&&(st.trigMode==='normal'||st.run==='single');}
function expectedLen(){
  if(isRoll()||trigWide()) return 1024;
  var W=10*TDIVS[st.tIdx]*expectedFs();
  return Math.max(64,Math.min(1024,Math.ceil(W*1.4)+32));
}

/* ---------- HTTP polling link ---------- */
/* serialized command queue with retry (a lost "run" used to leave Single armed forever) */
var cmdQ=[],cmdBusy=false,lastCmdMs=0;
function cmdKey(m){return (m==='run'||m==='stop')?'rs':m.split(':')[0];}
function send(m){
  var k=cmdKey(m);
  cmdQ=cmdQ.filter(function(c){return cmdKey(c.m)!==k;});
  cmdQ.push({m:m,t:0});
  lastCmdMs=performance.now();
  pump();
}
function pump(){
  if(cmdBusy||!cmdQ.length) return;
  cmdBusy=true;
  var c=cmdQ[0];
  fetch('/c?c='+encodeURIComponent(c.m),{cache:'no-store'}).then(function(r){
    if(!r.ok) throw new Error('http');
    var i=cmdQ.indexOf(c);if(i>=0)cmdQ.splice(i,1);
    cmdBusy=false;lastCmdMs=performance.now();pump();
  }).catch(function(){
    cmdBusy=false;
    var i=cmdQ.indexOf(c);
    if(i>=0&&++c.t>=6) cmdQ.splice(i,1);
    setTimeout(pump,150);
  });
}
/* compare what the ESP reports with what we want; repair on mismatch */
function syncState(r){
  if(cmdQ.length||cmdBusy||performance.now()-lastCmdMs<600) return;
  var xr=r.headers.get('X-Run'),xc=r.headers.get('X-Cfg');
  if(xr!==null){
    var want=(st.run!=='stop')?'1':'0';
    if(xr!==want){send(want==='1'?'run':'stop');return;}
  }
  if(xc!==null){
    var wantC=expectedFs()+','+expectedLen()+','+(isRoll()?1:0)+','+(st.src==='demo'?1:0);
    if(xc!==wantC){send('src:'+st.src);sendFs();}
  }
}
function sendFs(){ send('a:'+expectedFs()+','+expectedLen()+','+(isRoll()?1:0)); }
function sendCfg(){
  send('src:'+st.src); send('wave:'+$('selWave').value); send('freq:'+$('selFreq').value);
  sendFs(); send(st.run==='stop'?'stop':'run');
}
function onFrame(buf){
  if(st.run==='stop'||buf.byteLength<8) return;
  var dv=new DataView(buf),fs=dv.getUint32(0,true),meta=dv.getUint32(4,true),n=(buf.byteLength-8)>>1;
  var raw=new Uint16Array(buf,8,n);
  fpsCnt++;
  if(meta===1){ if(isRoll()) pushRoll(raw,fs); return; }
  if(isRoll()) return;
  if(st.skip>0){st.skip--;return;}          // frame captured before Single was armed
  last={fs:fs,raw:raw.slice(),v:null};
  process(last);
}
function resetRoll(){rollBuf=[];rollCount=0;rollTotal=0;rollPos=0;rollStartIdx=0;rollT=0;}
function modeSwitch(wasRoll){
  if(wasRoll||isRoll()){
    resetRoll();
    if(wasRoll!==isRoll()){last=null;disp=null;meas=null;updateMeas();}
  }
}
function pushRoll(raw,fs){
  if(rollFs!==fs){resetRoll();rollFs=fs;}
  var i,Wn=10*TDIVS[st.tIdx]*fs,keep=Math.ceil(Wn)+Math.ceil(fs*0.6)+4;
  for(i=0;i<raw.length;i++) rollBuf.push(raw[i]);
  rollTotal+=raw.length;
  if(rollBuf.length>keep) rollBuf.splice(0,rollBuf.length-keep);
  rollCount+=raw.length;
  processRoll();
  if(st.run==='single'&&rollCount>=Wn){st.run='stop';st.singleDone=true;send('stop');updateHeader();}
}
/* the screen scrolls on a smooth wall-clock (see tick), data only has to stay ahead of it */
function processRoll(){
  var fs=rollFs,Wn=10*TDIVS[st.tIdx]*fs;
  var f={fs:fs,raw:Uint16Array.from(rollBuf),v:null};
  var v=toVolts(f);f.v=v;last=f;
  st.clip=false;
  if(st.src==='adc'){for(var q=0;q<f.raw.length;q++){if(f.raw[q]>=1023){st.clip=true;break;}}}
  rollStartIdx=rollTotal-rollBuf.length;
  if(rollPos<=0) rollPos=rollTotal-Math.max(2,fs*0.12);
  disp={v:v,fs:fs,s:(rollPos-rollStartIdx)-Wn,Wn:Wn};
  st.trigState='ROLL';st.memN=v.length;
  meas=measure(v,fs);
  updateMeas();updateHeader();draw();
}
function tick(){
  requestAnimationFrame(tick);
  if(!isRoll()||st.run==='stop'||!disp||!rollBuf.length){rollT=0;return;}
  var now=performance.now(),dt=rollT?Math.min(0.1,(now-rollT)/1000):0;
  rollT=now;
  var fs=rollFs,target=rollTotal-Math.max(2,fs*0.12);
  if(Math.abs(target-rollPos)>fs*0.6) rollPos=target;
  else rollPos+=dt*fs+(target-rollPos)*0.04;
  if(rollPos>rollTotal-1) rollPos=rollTotal-1;
  disp.s=(rollPos-rollStartIdx)-disp.Wn;
  draw();
}
var fails=0;
function poll(){
  fetch('/d',{cache:'no-store'}).then(function(r){
    if(!connected){connected=true;setConn();sendCfg();draw();}
    fails=0;
    syncState(r);
    return r.status===200?r.arrayBuffer():null;
  }).then(function(buf){
    if(buf) onFrame(buf);
    setTimeout(poll,buf?2:(st.run==='stop'?200:(isRoll()?10:25)));
  }).catch(function(){
    fails++;
    if(fails>=3&&connected){connected=false;setConn();draw();}
    setTimeout(poll,500);
  });
}
function setConn(){
  $('led').className='led'+(connected?' on':'');
  $('conn').textContent=connected?'ONLINE':'OFFLINE';
}
setInterval(function(){$('fpsv').textContent=fpsCnt+' fps';fpsCnt=0;},1000);

/* ---------- signal processing ---------- */
function toVolts(f){
  var n=f.raw.length,v=new Float32Array(n),k=(st.src==='demo'?VREF:vrefAdc)/ADC_MAX*st.probe,i,sum=0;
  for(i=0;i<n;i++){v[i]=f.raw[i]*k;sum+=v[i];}
  if(st.cpl==='AC'){var m=sum/n;for(i=0;i<n;i++)v[i]-=m;}
  return v;
}
function findTrigger(v,fs){
  var n=v.length,Wn=10*TDIVS[st.tIdx]*fs;
  var preF=Wn*(0.5+st.hpos/10);
  var lo=Math.max(1,Math.ceil(preF)),hi=Math.min(n-1,Math.floor(n-1-(Wn-preF)));
  var L=st.trigV,i,a,b;
  for(i=lo;i<=hi;i++){
    a=v[i-1];b=v[i];
    if(st.slope>0?(a<L&&b>=L):(a>L&&b<=L)){
      return {found:true,s:(i-1+(L-a)/(b-a))-preF,Wn:Wn};
    }
  }
  return {found:false,s:0,Wn:Wn};
}
function measure(v,fs){
  var n=v.length,i,mn=Infinity,mx=-Infinity,sum=0,sq=0;
  for(i=0;i<n;i++){var x=v[i];if(x<mn)mn=x;if(x>mx)mx=x;sum+=x;sq+=x*x;}
  var r={min:mn,max:mx,vpp:mx-mn,avg:sum/n,rms:Math.sqrt(sq/n),freq:0,period:0};
  if(r.vpp>0.05*st.probe){
    var mid=(mx+mn)/2,hy=r.vpp*0.1,th=mid+hy,armed=v[0]<mid-hy,cr=[];
    for(i=1;i<n;i++){
      if(v[i]<mid-hy) armed=true;
      else if(armed&&v[i]>=th){cr.push(i-1+(th-v[i-1])/(v[i]-v[i-1]));armed=false;}
    }
    if(cr.length>=2){
      r.period=(cr[cr.length-1]-cr[0])/(cr.length-1)/fs;
      r.freq=1/r.period;
    }
  }
  return r;
}
/* tell the user WHY nothing triggers: level outside the signal range */
function waitState(v,base){
  var mn=Infinity,mx=-Infinity,i;
  for(i=0;i<v.length;i++){if(v[i]<mn)mn=v[i];if(v[i]>mx)mx=v[i];}
  return (st.trigV>mx||st.trigV<mn)?base+' LVL?':base;
}
function process(f,live){
  var v=toVolts(f);f.v=v;
  st.clip=false;
  if(st.src==='adc'){for(var q=0;q<f.raw.length;q++){if(f.raw[q]>=1023){st.clip=true;break;}}}
  var tr=findTrigger(v,f.fs),res;
  if(!tr.found&&st.force&&st.run!=='stop'){tr={found:true,s:0,Wn:tr.Wn,forced:true};st.force=false;}
  if(st.run==='single'){
    if(tr.found){res=tr;st.trigState=tr.forced?'FORCED':"TRIG'D";st.run='stop';st.singleDone=true;send('stop');}
    else{st.trigState=waitState(v,'ARMED');updateHeader();draw();return;}
  }else{
    if(tr.found){res=tr;st.trigState=tr.forced?'FORCED':"TRIG'D";}
    else if(st.trigMode==='auto'||st.run==='stop'){res={s:0,Wn:tr.Wn};st.trigState='AUTO';}
    else{st.trigState=waitState(v,'WAIT');updateHeader();draw();return;}
  }
  disp={v:v,fs:f.fs,s:res.s,Wn:res.Wn};
  st.memN=v.length;
  meas=measure(v,f.fs);
  updateMeas();updateHeader();draw();
}
function refresh(){
  if(isRoll()){
    if(rollBuf.length) processRoll();
    else{updateHeader();draw();}
    return;
  }
  if(!last||st.run==='single'){updateHeader();draw();return;}
  process(last,false);
}

/* ---------- UI update ---------- */
function updateMeas(){
  var m=meas,ok=m&&st.ch;
  $('mVpp').textContent=ok?eng(m.vpp,'V'):'--';
  $('mMax').textContent=ok?eng(m.max,'V'):'--';
  $('mMin').textContent=ok?eng(m.min,'V'):'--';
  $('mAvg').textContent=ok?eng(m.avg,'V'):'--';
  $('mRms').textContent=ok?eng(m.rms,'V'):'--';
  $('mFrq').textContent=(ok&&m.freq>0)?eng(m.freq,'Hz'):'--';
  $('mPer').textContent=(ok&&m.freq>0)?eng(m.period,'s'):'--';
}
function updateTrigSlider(){
  var vd=VDIVS[st.vIdx],s=$('trLvl');
  /* fixed range in volts: independent of POSITION, so the LEVEL thumb no longer jumps */
  s.min=-16*vd;s.max=16*vd;s.step=vd/50;s.value=st.trigV;
}
function updateHeader(){
  var r=$('stRun');
  r.textContent=st.run==='run'?'RUN':(st.run==='single'?'SINGLE':'STOP');
  r.className='badge '+st.run;
  $('stTrig').textContent=st.run==='stop'?(st.singleDone?'DONE':'HOLD'):st.trigState;
  $('stClip').style.display=st.clip?'':'none';
  $('hCH').textContent='CH1 '+eng(VDIVS[st.vIdx],'V')+'/div '+st.cpl+(st.probe>1?' x10':'');
  $('hH').textContent='H '+eng(TDIVS[st.tIdx],'s')+'/div';
  $('hT').textContent='T '+(st.slope>0?'\u2191':'\u2193')+' '+eng(st.trigV,'V');
  $('hFs').textContent='Fs '+(last?eng(last.fs,'Sa/s'):'--');
  $('hMem').textContent='Mem '+st.memN+' pts';
  $('bRun').className=st.run==='stop'?'stop':'run';
  $('bSingle').className=st.run==='single'?'armed':'';
}
function updateControls(){
  $('vVal').textContent=eng(VDIVS[st.vIdx],'V')+'/div';
  $('tVal').textContent=eng(TDIVS[st.tIdx],'s')+'/div';
  $('vPos').value=st.pos;$('hPos').value=st.hpos;
  updateTrigSlider();
  $('bCh').className=st.ch?'on':'off';$('bCh').textContent=st.ch?'CH1 ON':'CH1 OFF';
  $('bCpl').textContent=st.cpl;$('bProbe').textContent=st.probe+'X';
  $('bTAuto').className=st.trigMode==='auto'?'on':'';
  $('bTNorm').className=st.trigMode==='normal'?'on':'';
  $('bSlope').textContent=st.slope>0?'\u2191 RISE':'\u2193 FALL';
  $('bInterp').textContent='INTERP: '+(st.smooth?'SMOOTH':'LINEAR');$('bInterp').className=st.smooth?'on':'';
  $('bDemo').className=st.src==='demo'?'on':'';
  $('bAdc').className=st.src==='adc'?'on':'';
  $('selWave').className=$('selFreq').className=(st.src==='demo'?'':'dis');
  updateHeader();
}

/* ---------- drawing ---------- */
function layout(){
  var r=cv.parentNode.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
  var w=Math.max(100,Math.floor(r.width)),h=Math.max(100,Math.floor(r.height));
  cv.style.width=w+'px';cv.style.height=h+'px';
  if(cv.width!==Math.round(w*dpr)||cv.height!==Math.round(h*dpr)){
    cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);
  }
  cx.setTransform(dpr,0,0,dpr,0,0);
  return {w:w,h:h};
}
function tag(x,y,label,color,dir){
  cx.fillStyle=color;cx.beginPath();
  if(dir>0){cx.moveTo(x,y-7);cx.lineTo(x+13,y-7);cx.lineTo(x+19,y);cx.lineTo(x+13,y+7);cx.lineTo(x,y+7);}
  else{cx.moveTo(x+19,y-7);cx.lineTo(x+6,y-7);cx.lineTo(x,y);cx.lineTo(x+6,y+7);cx.lineTo(x+19,y+7);}
  cx.closePath();cx.fill();
  cx.fillStyle='#000';cx.font='bold 11px Consolas,monospace';cx.textAlign='center';cx.textBaseline='middle';
  cx.fillText(label,x+(dir>0?7:12),y+1);
}
function draw(){
  var L=layout(),W=L.w,H=L.h;
  cx.fillStyle='#000';cx.fillRect(0,0,W,H);
  var gx=28,gy=14,gw=W-gx-28,gh=H-gy-30;   /* bottom strip = scale read-out */
  if(gw<60||gh<60) return;
  var dx=gw/10,dy=gh/8,i,j,x,y;
  geom={gx:gx,gy:gy,gw:gw,gh:gh};
  /* grid */
  cx.lineWidth=1;cx.strokeStyle='#2c2c2c';cx.beginPath();
  for(i=1;i<10;i++){x=Math.round(gx+i*dx)+.5;cx.moveTo(x,gy);cx.lineTo(x,gy+gh);}
  for(j=1;j<8;j++){y=Math.round(gy+j*dy)+.5;cx.moveTo(gx,y);cx.lineTo(gx+gw,y);}
  cx.stroke();
  /* centre axes */
  var cxm=Math.round(gx+gw/2)+.5,cym=Math.round(gy+gh/2)+.5;
  cx.strokeStyle='#505050';cx.beginPath();
  cx.moveTo(cxm,gy);cx.lineTo(cxm,gy+gh);cx.moveTo(gx,cym);cx.lineTo(gx+gw,cym);cx.stroke();
  /* ticks (5 per division) on centre axes */
  cx.strokeStyle='#8a8a8a';cx.beginPath();
  for(i=0;i<=50;i++){x=gx+i*dx/5;var tl=(i%5===0)?5:3;cx.moveTo(x,cym-tl);cx.lineTo(x,cym+tl);}
  for(j=0;j<=40;j++){y=gy+j*dy/5;var tl2=(j%5===0)?5:3;cx.moveTo(cxm-tl2,y);cx.lineTo(cxm+tl2,y);}
  cx.stroke();
  cx.strokeStyle='#8a8a8a';cx.strokeRect(gx+.5,gy+.5,gw,gh);

  var vd=VDIVS[st.vIdx];
  function yOf(v){return gy+gh/2-((v/vd)+st.pos)*dy;}
  function clampY(y){return Math.max(gy+7,Math.min(gy+gh-7,y));}

  /* ---- scale labels: what one division means ---- */
  var small=W<560,td=TDIVS[st.tIdx];
  cx.font=(small?9:10)+'px Consolas,monospace';cx.textBaseline='alphabetic';
  if(st.ch){
    cx.fillStyle='rgba(255,212,0,0.55)';cx.textAlign='left';
    for(j=1;j<8;j++){
      var vv=((4-j)-st.pos)*vd;
      cx.fillText(Math.abs(vv)<vd*1e-6?'0 V':eng(vv,'V',3),gx+4,gy+j*dy-3);
    }
  }
  cx.fillStyle='rgba(210,210,210,0.55)';cx.textAlign='center';
  for(i=1;i<10;i++){
    if(dx<58&&i%2) continue;                       /* thin out labels on narrow screens */
    var tt=isRoll()?(i-10)*td:(i-(5+st.hpos))*td;
    cx.fillText(Math.abs(tt)<td*1e-6?'0':eng(tt,'s',3),gx+i*dx,gy+gh-4);
  }
  /* bottom read-out strip */
  var by=H-9;
  cx.font='bold '+(small?11:13)+'px Consolas,monospace';
  cx.textAlign='left';cx.fillStyle='#ffd400';
  cx.fillText(st.ch?('CH1  '+eng(vd,'V')+'/div'+(st.probe>1?'  \u00d710':'')):'CH1 OFF',gx,by);
  cx.textAlign='center';cx.fillStyle='#f0f0f0';
  cx.fillText('H  '+eng(td,'s')+'/div'+(isRoll()?'  ROLL':''),gx+gw*(small?0.68:0.5),by);
  if(!small){
    cx.textAlign='right';cx.fillStyle='#8a8a8a';cx.font='12px Consolas,monospace';
    cx.fillText('8 div = '+eng(8*vd,'V')+'    10 div = '+eng(10*td,'s'),gx+gw,by);
  }

  /* trigger level (faint dashed) */
  if(st.ch){
    var ty=yOf(st.trigV);
    if(ty>=gy&&ty<=gy+gh){
      cx.save();cx.setLineDash([4,5]);cx.strokeStyle='rgba(255,212,0,0.30)';
      cx.beginPath();cx.moveTo(gx,ty);cx.lineTo(gx+gw,ty);cx.stroke();cx.restore();
    }
  }
  /* trace */
  if(st.ch&&disp&&disp.Wn>0){
    cx.save();cx.beginPath();cx.rect(gx,gy,gw,gh);cx.clip();
    cx.strokeStyle='#ffd400';cx.lineWidth=1.6;cx.lineJoin='round';cx.beginPath();
    var v=disp.v,n=v.length,s=disp.s,Wn=disp.Wn;
    var j0=Math.max(0,Math.floor(s)),j1=Math.min(n-1,Math.ceil(s+Wn)),first=true;
    var pps=gw/Wn;
    if(st.smooth&&pps>=4&&!isRoll()){
      /* Catmull-Rom interpolation between samples: fine-scale view looks like a smooth curve */
      var mSub=Math.min(16,Math.max(2,Math.ceil(pps/3)));
      for(j=j0;j<=j1;j++){
        x=gx+(j-s)/Wn*gw;
        if(first){cx.moveTo(x,yOf(v[j]));first=false;continue;}
        var xp=gx+(j-1-s)/Wn*gw;
        var q0=v[Math.max(0,j-2)],q1=v[j-1],q2=v[j],q3=v[Math.min(n-1,j+1)];
        for(var kk=1;kk<=mSub;kk++){
          var u=kk/mSub,u2=u*u,u3=u2*u;
          var yy=0.5*((2*q1)+(-q0+q2)*u+(2*q0-5*q1+4*q2-q3)*u2+(-q0+3*q1-3*q2+q3)*u3);
          cx.lineTo(xp+(x-xp)*u,yOf(yy));
        }
      }
    }else{
      for(j=j0;j<=j1;j++){
        x=gx+(j-s)/Wn*gw;y=yOf(v[j]);
        if(first){cx.moveTo(x,y);first=false;}else cx.lineTo(x,y);
      }
    }
    cx.stroke();
    if(gw/Wn>=7){
      cx.fillStyle='#ffd400';
      for(j=j0;j<=j1;j++){x=gx+(j-s)/Wn*gw;y=yOf(v[j]);cx.fillRect(x-1.5,y-1.5,3,3);}
    }
    cx.restore();
  }
  /* markers */
  if(st.ch){
    tag(gx-23,clampY(yOf(0)),'1','#ffd400',1);
    tag(gx+gw+5,clampY(yOf(st.trigV)),'T','#ffd400',-1);
  }
  var tx=gx+gw*(0.5+st.hpos/10);tx=Math.max(gx+6,Math.min(gx+gw-6,tx));
  cx.fillStyle='#ff8a00';cx.beginPath();cx.moveTo(tx-6,2);cx.lineTo(tx+6,2);cx.lineTo(tx,11);cx.closePath();cx.fill();
  /* overlay */
  if(!connected||!disp){
    cx.fillStyle='rgba(255,255,255,0.45)';cx.font='15px Consolas,monospace';
    cx.textAlign='center';cx.textBaseline='middle';
    cx.fillText(connected?'WAITING FOR DATA...':'CONNECTING TO SCOPE...',gx+gw/2,gy+gh/2-30);
  }
}

/* ---------- controls ---------- */
function setRun(m){
  st.run=m;st.force=false;st.singleDone=false;
  st.skip=(m==='single'&&!isRoll())?1:0;
  if((m==='run'||m==='single')&&isRoll()) resetRoll();
  if(m!=='stop') sendFs();                  // single/normal use a wider capture; also restarts acquisition cleanly
  send(m==='stop'?'stop':'run');
  if(m==='single')st.trigState='ARMED';
  updateHeader();
  if(m==='stop') refresh();
}
function nearestT(t){
  var b=0,bd=1e9,i;
  for(i=0;i<TDIVS.length;i++){var d=Math.abs(Math.log(TDIVS[i]/t));if(d<bd){bd=d;b=i;}}
  return b;
}
function autoScale(){
  if(!last||!last.v) return;
  var v=last.v,m=measure(v,last.fs);
  if(m.freq>0){var w0=isRoll();st.tIdx=nearestT(0.3/m.freq);clampT();modeSwitch(w0);sendFs();}
  var mid=(m.max+m.min)/2,need=Math.max(m.vpp/6,1e-6);
  need=Math.max(need,Math.abs(mid)/10);
  var k=VDIVS.length-1,i;
  for(i=0;i<VDIVS.length;i++){if(VDIVS[i]>=need){k=i;break;}}
  st.vIdx=k;
  st.pos=Math.max(-12,Math.min(12,-mid/VDIVS[k]));
  st.trigV=mid;st.hpos=0;st.trigMode='auto';st.slope=1;
  updateControls();
  setRun('run');
  refresh();
}
function step(key,dir,len,after){
  st[key]=Math.max(0,Math.min(len-1,st[key]+dir));
  after();
}
function onV(d){step('vIdx',d,VDIVS.length,function(){updateControls();refresh();});}
function onT(d){var w=isRoll();step('tIdx',d,TDIVS.length,function(){clampT();modeSwitch(w);sendFs();updateControls();refresh();});}

$('bRun').onclick=function(){setRun(st.run==='stop'?'run':'stop');};
$('bSingle').onclick=function(){setRun('single');};
$('bAuto').onclick=autoScale;
$('bForce').onclick=function(){
  if(st.run==='stop') return;
  if(isRoll()){
    if(st.run==='single'&&rollBuf.length){st.run='stop';st.singleDone=true;send('stop');updateHeader();}
    return;
  }
  st.force=true;                            // accept the next frame even without a trigger edge
};
$('bCh').onclick=function(){st.ch=!st.ch;updateControls();updateMeas();draw();};
$('bCpl').onclick=function(){st.cpl=st.cpl==='DC'?'AC':'DC';
  st.trigV=(st.cpl==='AC')?0:((st.src==='demo'?VREF:vrefAdc)*st.probe/2);
  updateControls();refresh();};
$('bProbe').onclick=function(){st.probe=st.probe===1?10:1;updateControls();refresh();};
$('vDn').onclick=function(){onV(-1);};$('vUp').onclick=function(){onV(1);};
$('tDn').onclick=function(){onT(-1);};$('tUp').onclick=function(){onT(1);};
$('vPos').oninput=function(){st.pos=parseFloat(this.value);updateTrigSlider();refresh();};
$('hPos').oninput=function(){st.hpos=parseFloat(this.value);refresh();};
$('vPos').ondblclick=function(){st.pos=-3;updateControls();refresh();};
$('hPos').ondblclick=function(){st.hpos=0;updateControls();refresh();};
$('trLvl').oninput=function(){st.trigV=parseFloat(this.value);refresh();};
$('bTAuto').onclick=function(){st.trigMode='auto';sendFs();updateControls();refresh();};
$('bTNorm').onclick=function(){st.trigMode='normal';sendFs();updateControls();refresh();};
$('bSlope').onclick=function(){st.slope=-st.slope;updateControls();refresh();};
function bench(){
  $('adcInfo').textContent='ADC test running...';
  fetch('/c?c=bench',{cache:'no-store'}).then(function(r){return r.json();}).then(function(o){
    if(o.adcMax>0) FS_MAX.adc=o.adcMax;
    $('adcInfo').textContent='ADC max '+eng(o.rate,'Sa/s')+' | Heap '+(o.heap/1024).toFixed(1)+' KB';
    if(st.src==='adc'){clampT();sendFs();updateControls();}
  }).catch(function(){$('adcInfo').textContent='ADC test failed';});
}
function setSrc(s){
  st.src=s;last=null;disp=null;meas=null;st.clip=false;resetRoll();
  send('src:'+s);clampT();sendFs();updateControls();updateMeas();
  if(s==='adc') bench();
  draw();
}
$('bBench').onclick=bench;
$('bInterp').onclick=function(){st.smooth=!st.smooth;updateControls();draw();};
try{var sv=parseFloat(localStorage.getItem('vrefAdc'));if(sv>=2&&sv<=4)vrefAdc=sv;}catch(e){}
$('vref').value=vrefAdc;
$('vref').onchange=function(){
  var x=parseFloat(this.value);
  if(x>=2&&x<=4){vrefAdc=x;try{localStorage.setItem('vrefAdc',String(x));}catch(e){}refresh();}
  else this.value=vrefAdc;
};
/* drag on screen edges: left = POSITION (ground marker), right = trigger LEVEL */
function dragMove(e){
  if(!geom||!drag) return;
  var r=cv.getBoundingClientRect(),y=e.clientY-r.top;
  var dv=(geom.gy+geom.gh/2-y)/(geom.gh/8),vd=VDIVS[st.vIdx];
  if(drag==='trig'){st.trigV=(dv-st.pos)*vd;}
  else{st.pos=Math.max(-12,Math.min(12,dv));}
  $('vPos').value=st.pos;updateTrigSlider();updateHeader();refresh();
}
cv.addEventListener('pointerdown',function(e){
  if(!geom) return;
  var r=cv.getBoundingClientRect(),x=e.clientX-r.left;
  if(x>geom.gx+geom.gw-8) drag='trig';
  else if(x<geom.gx+8) drag='pos';
  else return;
  try{cv.setPointerCapture(e.pointerId);}catch(x2){}
  dragMove(e);
});
cv.addEventListener('pointermove',function(e){
  if(drag){dragMove(e);return;}
  if(!geom) return;
  var r=cv.getBoundingClientRect(),x=e.clientX-r.left;
  cv.style.cursor=(x>geom.gx+geom.gw-8||x<geom.gx+8)?'ns-resize':'default';
});
cv.addEventListener('pointerup',function(){drag=null;});
cv.addEventListener('pointercancel',function(){drag=null;});
$('bDemo').onclick=function(){setSrc('demo');};
$('bAdc').onclick=function(){setSrc('adc');};
$('selWave').onchange=function(){send('wave:'+this.value);};
$('selFreq').onchange=function(){send('freq:'+this.value);};
document.addEventListener('keydown',function(e){
  var t=e.target&&e.target.tagName;
  if(t==='SELECT'||t==='INPUT') return;
  if(e.code==='Space'){e.preventDefault();setRun(st.run==='stop'?'run':'stop');}
  else if(e.key==='s'||e.key==='S'){setRun('single');}
  else if(e.key==='a'||e.key==='A'){autoScale();}
});
window.addEventListener('resize',draw);
if(window.ResizeObserver) new ResizeObserver(draw).observe($('scopeWrap'));

updateControls();setConn();draw();poll();requestAnimationFrame(tick);
})();
</script>
</body>
</html>
)=====";

// ------------------------- acquisition state ---------------------
// The browser tells the ESP (command "a:<fs>,<len>,<roll>"):
//   fs   : sample rate matching the Time/div setting
//   len  : samples needed for ONE screen (+ trigger margin), not the full buffer
//   roll : 1 = slow timebase -> continuous streaming (roll mode), no trigger
uint16_t capLen = 1024;
bool     rollMode = false;
bool     rollActive = false;
uint32_t rollHead = 0, rollTail = 0, rollNextUs = 0, rollPeriod = 0, rollFs = 0, lastRollSend = 0;
double   rollBase = 0;
uint16_t txLen = 0;

// ------------------------- helpers -------------------------------
uint32_t fsLimit() { return demoMode ? FS_MAX_DEMO : fsMaxAdc; }

// frame layout: [uint32 fs][uint32 meta (0 = full frame, 1 = roll chunk)][n x uint16]
void storeFrame(uint32_t fs, uint32_t meta, const uint16_t* data, uint16_t n) {
  memcpy(txBuf, &fs, 4);
  memcpy(txBuf + 4, &meta, 4);
  memcpy(txBuf + 8, data, 2 * (size_t)n);
  txLen = 8 + 2 * n;
  frameReady = true;
}

void sendFrame(uint32_t fs, uint16_t n) { storeFrame(fs, 0, sampleBuf, n); }

// ------------------------- demo signal ---------------------------
float waveAt(double ph) {
  if (demoWave == 0) return sinf((float)(6.2831853 * ph));
  if (demoWave == 1) return (ph < 0.5) ? 1.0f : -1.0f;
  return (ph < 0.5) ? (float)(4.0 * ph - 1.0) : (float)(3.0 - 4.0 * ph);
}

uint16_t toRaw(float volts) {
  int raw = (int)(volts / 3.3f * 1023.0f + 0.5f);
  if (raw < 0) raw = 0;
  if (raw > 1023) raw = 1023;
  return (uint16_t)raw;
}

void demoStep() {
  uint32_t fs = fsReq;
  if (fs > fsLimit()) fs = fsLimit();
  if (fs < 10) fs = 10;
  uint16_t L = capLen;
  if (L < 32) L = 32;
  if (L > N_SAMPLES) L = N_SAMPLES;

  uint32_t acqMs = (uint32_t)((uint64_t)L * 1000ULL / fs);
  uint32_t interval = acqMs < 40 ? 40 : acqMs;
  if (interval > 3000) interval = 3000;

  uint32_t now = millis();
  if (now - lastFrameMs < interval) return;
  lastFrameMs = now;

  double t0 = (double)micros() * 1e-6;
  double ph0 = fmod(t0 * demoFreq, 1.0);
  double dph = (double)demoFreq / (double)fs;

  for (uint16_t i = 0; i < L; i++) {
    double ph = ph0 + i * dph;
    ph -= floor(ph);
    float volts = 1.65f + 1.0f * waveAt(ph) + (float)random(-10, 11) * 0.001f;
    sampleBuf[i] = toRaw(volts);
  }
  sendFrame(fs, L);
}

// ------------------------- roll mode (slow timebases) ------------------
// Samples are written continuously into a ring (sampleBuf); every browser
// poll takes everything that is new.  If the browser is too slow the oldest
// samples are dropped, so memory can never overflow.
void rollStep() {
  uint32_t fs = fsReq;
  if (fs > fsLimit()) fs = fsLimit();
  if (fs < 10) fs = 10;

  if (!rollActive) {
    rollActive = true;
    rollFs = fs;
    rollPeriod = 1000000UL / fs;
    rollHead = rollTail = 0;
    rollNextUs = micros();
    rollBase = (double)micros() * 1e-6;
  }
  for (uint8_t k = 0; k < 16; k++) {
    if ((int32_t)(micros() - rollNextUs) < 0) break;
    uint16_t v;
    if (demoMode) {
      double t = rollBase + (double)rollHead / (double)rollFs;
      double ph = fmod(t * demoFreq, 1.0);
      v = toRaw(1.65f + 1.0f * waveAt(ph) + (float)random(-10, 11) * 0.001f);
    } else {
      v = (uint16_t)analogRead(A0);
    }
    sampleBuf[rollHead % N_SAMPLES] = v;
    rollHead++;
    rollNextUs += rollPeriod;
  }
  if ((int32_t)(micros() - rollNextUs) > (int32_t)(rollPeriod * 8)) rollNextUs = micros();
}

// ------------------------- real ADC (A0) -------------------------
// Fast timebases : blocking burst capture (accurate sample spacing).
// Medium         : non-blocking state machine so WiFi keeps running.
const uint32_t BURST_MIN_FS = 3500;   // L<=1024 -> at most ~290 ms blocking

void adcBurst(uint32_t fs, uint16_t L) {
  uint32_t period = 1000000UL / fs;
  uint32_t start = micros(), next = start;
  for (uint16_t i = 0; i < L; i++) {
    while ((int32_t)(micros() - next) < 0) { }
    sampleBuf[i] = (uint16_t)analogRead(A0);
    next += period;
    if ((int32_t)(micros() - next) > 0) next = micros();  // ADC slower than requested
  }
  uint32_t elapsed = micros() - start;
  if (elapsed == 0) elapsed = 1;
  sendFrame((uint32_t)((uint64_t)L * 1000000ULL / elapsed), L);  // report the REAL rate
  lastFrameMs = millis();
}

void adcStep() {
  uint32_t fs = fsReq;
  if (fs > fsLimit()) fs = fsLimit();
  if (fs < 10) fs = 10;
  uint16_t L = capLen;
  if (L < 32) L = 32;
  if (L > N_SAMPLES) L = N_SAMPLES;

  if (fs >= BURST_MIN_FS) {
    acqActive = false;
    if (frameReady) return;                       // previous frame not fetched yet
    if (millis() - lastFrameMs < 15) return;      // leave time for HTTP
    adcBurst(fs, L);
    return;
  }

  if (!acqActive) {
    acqPeriod = 1000000UL / fs;
    acqIdx = 0;
    acqStart = acqNext = micros();
    acqActive = true;
  }
  uint32_t now = micros();
  if ((int32_t)(now - acqNext) < 0) return;

  sampleBuf[acqIdx++] = (uint16_t)analogRead(A0);
  acqNext += acqPeriod;
  if ((int32_t)(micros() - acqNext) > (int32_t)(acqPeriod * 4)) acqNext = micros();  // fell behind

  if (acqIdx >= L) {
    uint32_t elapsed = micros() - acqStart;
    if (elapsed == 0) elapsed = 1;
    acqActive = false;
    sendFrame((uint32_t)((uint64_t)L * 1000000ULL / elapsed), L);
  }
}

// ------------------------- commands ------------------------------
void resetAcq() {
  acqActive = false;
  rollActive = false;
  frameReady = false;
}

void applyCmd(const String& cmd) {
  if (cmd == "run") {
    running = true;
  } else if (cmd == "stop") {
    running = false;
    resetAcq();
  } else if (cmd.startsWith("a:")) {            // a:<fs>,<len>,<roll>
    unsigned long f = 0, l = 0, r = 0;
    if (sscanf(cmd.c_str() + 2, "%lu,%lu,%lu", &f, &l, &r) == 3) {
      if (f < 10) f = 10;
      if (l < 32) l = 32;
      if (l > N_SAMPLES) l = N_SAMPLES;
      fsReq = (uint32_t)f;
      capLen = (uint16_t)l;
      rollMode = (r != 0);
      resetAcq();
    }
  } else if (cmd.startsWith("fs:")) {
    long v = cmd.substring(3).toInt();
    if (v < 10) v = 10;
    fsReq = (uint32_t)v;
    resetAcq();
  } else if (cmd == "src:demo") {
    demoMode = true;
    resetAcq();
  } else if (cmd == "src:adc") {
    demoMode = false;
    resetAcq();
  } else if (cmd.startsWith("wave:")) {
    int w = cmd.substring(5).toInt();
    demoWave = (w < 0 || w > 2) ? 0 : (uint8_t)w;
  } else if (cmd.startsWith("freq:")) {
    long f = cmd.substring(5).toInt();
    if (f >= 1 && f <= 20000) demoFreq = (float)f;
  }
}

// ------------------------- http ----------------------------------
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleData() {
  lastPollMs = millis();
  server.sendHeader("Cache-Control", "no-store");
  // report state so the browser can detect and repair a lost command
  server.sendHeader("X-Run", running ? "1" : "0");
  server.sendHeader("X-Cfg", String(fsReq) + "," + String(capLen) + "," + String(rollMode ? 1 : 0) + "," + String(demoMode ? 1 : 0));

  if (rollMode) {                         // roll mode: hand out everything that is new
    uint32_t pend = rollHead - rollTail;
    if (pend > 0 && (millis() - lastRollSend) >= 15) {
      if (pend > N_SAMPLES) {             // browser too slow: drop the oldest samples
        rollTail = rollHead - N_SAMPLES;
        pend = N_SAMPLES;
      }
      uint32_t fs = rollFs, meta = 1;
      memcpy(txBuf, &fs, 4);
      memcpy(txBuf + 4, &meta, 4);
      uint16_t* out = (uint16_t*)(txBuf + 8);
      for (uint32_t k = 0; k < pend; k++) out[k] = sampleBuf[(rollTail + k) % N_SAMPLES];
      rollTail += pend;
      txLen = 8 + 2 * pend;
      lastRollSend = millis();
      server.send_P(200, "application/octet-stream", (PGM_P)txBuf, txLen);
    } else {
      server.send(204, "text/plain", "");
    }
    return;
  }

  if (frameReady) {
    frameReady = false;
    server.send_P(200, "application/octet-stream", (PGM_P)txBuf, txLen);
  } else {
    server.send(204, "text/plain", "");
  }
}

void handleCmd() {
  lastPollMs = millis();
  String c = server.arg("c");
  server.sendHeader("Cache-Control", "no-store");

  if (c == "bench") {                     // ADC speed test + free heap
    resetAcq();
    const int K = 1000;
    volatile uint32_t sum = 0;
    uint32_t t = micros();
    for (int i = 0; i < K; i++) sum += analogRead(A0);
    uint32_t el = micros() - t;
    if (el == 0) el = 1;
    uint32_t rate = (uint32_t)((uint64_t)K * 1000000ULL / el);
    uint32_t m = rate * 9 / 10;
    if (m < 200) m = 200;
    if (m > 100000) m = 100000;
    fsMaxAdc = m;
    frameReady = false;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"rate\":%u,\"adcMax\":%u,\"heap\":%u}",
             (unsigned)rate, (unsigned)m, (unsigned)ESP.getFreeHeap());
    Serial.printf("[ADC test] %u Sa/s, heap %u\n", (unsigned)rate, (unsigned)ESP.getFreeHeap());
    server.send(200, "application/json", String(buf));
    return;
  }

  applyCmd(c);
  server.send(200, "text/plain", "ok");
}

void handleNoContent() {
  server.send(204, "text/plain", "");
}

void handleNotFound() {                // captive portal: send every unknown URL to the scope page
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(302, "text/plain", "");
}

// ------------------------- setup / loop --------------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  system_update_cpu_freq(160);  // 160 MHz for faster waveform generation

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);
  Serial.println();
  Serial.print("AP ");
  Serial.print(AP_SSID);
  Serial.println(ok ? " started" : " FAILED");
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/d", handleData);
  server.on("/c", handleCmd);
  server.on("/favicon.ico", handleNoContent);
  server.onNotFound(handleNotFound);
  server.begin();

  // captive portal: answer EVERY DNS name with our own IP so the phone/PC
  // detects "login required" and opens the scope page automatically
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);

  randomSeed(micros());
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // acquire only while a browser is polling
  if (!running || (millis() - lastPollMs) > 2000) {
    acqActive = false;
    rollActive = false;
    return;
  }
  if (rollMode) {
    rollStep();
  } else {
    rollActive = false;
    if (demoMode) demoStep();
    else adcStep();
  }
}
