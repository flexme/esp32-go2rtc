/* Metadata-only benchmarking. No image/audio payload or credentials are retained. */
'use strict';
class XrtcDiagnostics {
  constructor(video, request, session) {
    this.video = video; this.request = request; this.session = session;
    this.started = performance.now(); this.clocks = []; this.samples = []; this.latencies = [];
    this.nativeFrames = []; this.seenNative = new Set();
    this.native = new Map(); this.presented = new Map(); this.matched = new Set();
    this.offset = null; this.clockRtt = null; this.running = false; this.callback = null;
    this.firstPresentedMs = null; this.rtpMetadata = false; this.truncated = false;
  }
  static videoStats(stats) {
    const fields = ['framesReceived','framesDecoded','framesDropped','keyFramesDecoded',
      'frameWidth','frameHeight','framesPerSecond','bytesReceived','packetsReceived','packetsLost',
      'jitter','jitterBufferDelay','jitterBufferEmittedCount','totalDecodeTime',
      'freezeCount','totalFreezesDuration','pliCount','nackCount'];
    const result = [];
    stats.forEach(s => {
      if (s.type !== 'inbound-rtp' || s.kind !== 'video') return;
      const v = {codec:stats.get(s.codecId)?.mimeType};
      for (const key of fields) if (s[key] !== undefined) v[key] = s[key];
      result.push(v);
    });
    return result;
  }
  static distribution(values) {
    if (!values.length) return {count:0};
    const a = [...values].sort((x,y)=>x-y);
    const q = p => a[Math.min(a.length-1,Math.floor((a.length-1)*p))];
    return {count:a.length,min:a[0],p50:q(.5),p95:q(.95),p99:q(.99),max:a.at(-1),
      mean:a.reduce((x,y)=>x+y,0)/a.length};
  }
  async synchronize() {
    let best = null;
    for (let i=0;i<5;i++) {
      const t0 = performance.now(), reply = await this.request('clock'), t1 = performance.now();
      const clock = {browser_ms:(t0+t1)/2,offset_ms:reply.mono_ms-(t0+t1)/2,rtt_ms:t1-t0};
      if (!best || clock.rtt_ms < best.rtt_ms) best = clock;
    }
    this.offset = best.offset_ms; this.clockRtt = best.rtt_ms; this.clocks.push(best);
    if (this.clocks.length > 360) this.clocks.shift();
  }
  async start() {
    await this.synchronize(); this.running = true;
    if (this.video.requestVideoFrameCallback) {
      const observe = (now, meta) => {
        if (!this.running) return;
        if (this.firstPresentedMs === null) this.firstPresentedMs = now-this.started;
        if (Number.isFinite(meta.rtpTimestamp)) {
          this.rtpMetadata = true;
          const rtp = meta.rtpTimestamp >>> 0;
          this.presented.set(rtp, {display_ms:meta.expectedDisplayTime,seen_ms:now,
            processing_ms:meta.processingDuration === undefined ? null : meta.processingDuration*1000});
          this.match(rtp);
        }
        this.callback = this.video.requestVideoFrameCallback(observe);
      };
      this.callback = this.video.requestVideoFrameCallback(observe);
    }
  }
  match(rtp) {
    if (this.matched.has(rtp) || this.offset === null) return;
    const n = this.native.get(rtp), p = this.presented.get(rtp);
    if (!n || !p) return;
    // expectedDisplayTime is the compositor's presentation estimate, not an
    // optical sensor-to-screen measurement. HTTP clock error is <= RTT/2
    // under the usual nonnegative path-delay assumption, plus clock drift.
    const ingress = n[1]/1000-this.offset, relay = (n[2]-n[1])/1000;
    this.latencies.push({rtp,elapsed_ms:p.seen_ms-this.started,
      ingress_to_display_ms:p.display_ms-ingress,relay_ms:relay,
      send_to_display_ms:p.display_ms-(n[2]/1000-this.offset),bytes:n[3],
      decode_processing_ms:p.processing_ms,clock_rtt_ms:this.clockRtt});
    this.matched.add(rtp); this.native.delete(rtp); this.presented.delete(rtp);
    if (this.latencies.length > 36000) { this.latencies.shift(); this.truncated = true; }
    if (this.matched.size > 72000) this.matched.delete(this.matched.values().next().value);
  }
  async sample(stats) {
    if (!this.running) return;
    if (performance.now()-this.clocks.at(-1).browser_ms > 60000) await this.synchronize();
    const board = await this.request('status'), timings = await this.request('rtc/timings');
    if (timings.session !== this.session) throw Error('Viewer session changed during benchmark');
    for (const n of timings.frames) {
      if (!this.seenNative.has(n[0])) {
        this.seenNative.add(n[0]); this.nativeFrames.push(n);
        if (this.nativeFrames.length > 36000) { this.nativeFrames.shift(); this.truncated = true; }
        if (this.seenNative.size > 72000) this.seenNative.delete(this.seenNative.values().next().value);
      }
      if (!this.matched.has(n[0])) { this.native.set(n[0],n); this.match(n[0]); }
    }
    // A stalled decoder must not grow browser memory without a bound.
    for (const map of [this.native,this.presented])
      while (map.size > 4096) map.delete(map.keys().next().value);
    const quality = this.video.getVideoPlaybackQuality?.();
    this.samples.push({elapsed_ms:performance.now()-this.started,utc_ms:Date.now(),
      video:XrtcDiagnostics.videoStats(stats),board,
      playback:quality ? {totalVideoFrames:quality.totalVideoFrames,
        droppedVideoFrames:quality.droppedVideoFrames,corruptedVideoFrames:quality.corruptedVideoFrames} : null});
    if (this.samples.length > 3600) { this.samples.shift(); this.truncated = true; }
  }
  summary() {
    return {elapsed_seconds:(performance.now()-this.started)/1000,
      first_presented_ms:this.firstPresentedMs,rtp_frame_metadata:this.rtpMetadata,
      clock_rtt_ms:this.clockRtt,
      latency_definition:'Complete encrypted frame at ESP to estimated browser presentation; excludes camera capture/encode and camera-to-ESP transfer.',
      ingress_to_display_ms:XrtcDiagnostics.distribution(this.latencies.map(x=>x.ingress_to_display_ms)),
      relay_ms:XrtcDiagnostics.distribution(this.latencies.map(x=>x.relay_ms)),
      all_sent_relay_ms:XrtcDiagnostics.distribution(this.nativeFrames.map(x=>(x[2]-x[1])/1000)),
      send_to_display_ms:XrtcDiagnostics.distribution(this.latencies.map(x=>x.send_to_display_ms)),
      samples:this.samples.length,pending_presented:this.presented.size,pending_native:this.native.size,
      truncated:this.truncated};
  }
  report() {
    return {format:'xiaomi-rtc-benchmark-v1',summary:this.summary(),clocks:this.clocks,
      samples:this.samples,latencies:this.latencies,native_frames:this.nativeFrames};
  }
  stop() {
    this.running = false;
    if (this.callback !== null) this.video.cancelVideoFrameCallback?.(this.callback);
  }
}
if (typeof module !== 'undefined') module.exports = {XrtcDiagnostics};
