'use strict';
const assert = require('node:assert/strict');
const {XrtcDiagnostics} = require('../main/www/diagnostics.js');

// The board clock is 50 ms ahead; an encrypted frame completes at board
// 100 ms, is sent at 130 ms and displayed at browser 120 ms.
const d = new XrtcDiagnostics({},null,1);
d.started = 0; d.offset = 50; d.clockRtt = 2;
d.presented.set(0xfffffff0,{display_ms:120,seen_ms:121,processing_ms:4});
d.native.set(0xfffffff0,[0xfffffff0,100000,130000,2000]);
d.match(0xfffffff0);
assert.equal(d.latencies[0].ingress_to_display_ms,70);
assert.equal(d.latencies[0].relay_ms,30);
assert.equal(d.latencies[0].send_to_display_ms,40);
// Polling the same ring entry must not double-count a displayed frame.
d.match(0xfffffff0); assert.equal(d.latencies.length,1);
// RTP rollover is an identifier, not a signed duration calculation.
d.native.set(16,[16,200000,210000,4000]);
d.presented.set(16,{display_ms:200,seen_ms:201,processing_ms:3});
d.match(16);assert.equal(d.latencies[1].ingress_to_display_ms,50);
assert.equal(d.summary().ingress_to_display_ms.count,2);
assert.equal(d.summary().ingress_to_display_ms.mean,60);
assert.deepEqual(XrtcDiagnostics.distribution([]),{count:0});
assert.equal(d.summary().relay_ms.max,30);
console.log('Clock-domain conversion, frame join, deduplication and RTP wrap: PASS');
