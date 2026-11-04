// SPDX-License-Identifier: BSD-3-Clause
// Tests the page script coretube_sabr.js against fake browser objects.
// Usage: node sabr_script_test.js SCRIPT
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync(process.argv[2], 'utf8');

// A page: fake time, timers, events, media elements, fetch and AbortController.
function page(extra = {}) {
  const p = { t: 0, logs: [], timers: [], seen: [] };
  const listeners = { window: {}, document: {} };
  const on = (where) => (type, fn) => { (listeners[where][type] = listeners[where][type] || []).push(fn); };

  // Capture listeners on window, then document, then the page's own on the target.
  p.dispatch = (type, target) => {
    const e = { type, target, stopped: false, stopImmediatePropagation() { this.stopped = true; } };
    for (const where of ['window', 'document']) {
      for (const fn of listeners[where][type] || []) {
        fn(e);
        if (e.stopped) return;
      }
    }
    p.seen.push([type, target.id]);
  };

  class HTMLMediaElement {
    constructor(id) { this.id = id; this.paused = false; this.ended = false; this.currentTime = 0; this.duration = 600; this.ranges = []; this.isConnected = true; }
    get buffered() { const r = this.ranges; return { length: r.length, start: (i) => r[i][0], end: (i) => r[i][1] }; }
  }
  HTMLMediaElement.prototype.pause = function () { this.paused = true; p.dispatch('pause', this); };
  HTMLMediaElement.prototype.play = function () { this.paused = false; p.dispatch('play', this); return Promise.resolve(); };
  class HTMLVideoElement extends HTMLMediaElement {}

  class AbortController {
    constructor() { this.signal = { aborted: false }; }
  }
  AbortController.prototype.abort = function () { this.signal.aborted = true; };
  class Request { constructor(url, init) { this.url = url; this.signal = init && init.signal; } }

  // A download: the test pushes chunks into it.
  p.downloads = [];
  const fetch = (input, init) => {
    const d = { url: typeof input === 'string' ? input : input.url, pending: [] };
    d.push = (bytes) => { const w = d.pending.shift(); if (w) w({ done: false, value: new Uint8Array(bytes) }); else d.queued = (d.queued || 0) + 1; };
    d.finish = () => { const w = d.pending.shift(); if (w) w({ done: true }); };
    p.downloads.push(d);
    const body = { getReader: () => ({ read: () => new Promise((res) => d.pending.push(res)) }) };
    return Promise.resolve({ clone: () => ({ body }) });
  };

  // One-shot timers run when the test calls p.runTimeouts().
  p.timeouts = [];
  p.runTimeouts = () => { const due = p.timeouts.splice(0); due.forEach((fn) => fn()); };

  const ctx = {
    console: { info: (m) => p.logs.push(m) },
    performance: { now: () => p.t },
    location: { hostname: 'www.youtube.com', hash: '#/watch?v=abc' },
    localStorage: {},
    Map,  // the test's own, so the script's instanceof Map holds for the test's maps
    setTimeout: (fn) => { p.timeouts.push(fn); },
    setInterval: (fn) => { p.timers.push(fn); return p.timers.length; },
    clearInterval: (id) => { p.timers[id - 1] = null; },
    document: { addEventListener: on('document') },
    HTMLMediaElement, HTMLVideoElement, AbortController, Request, fetch,
  };
  ctx.window = ctx;
  ctx.window.addEventListener = on('window');
  Object.assign(ctx, extra);
  vm.runInNewContext(source, ctx);
  p.ctx = ctx;
  // Time passes in 250 ms steps; interval timers run each step.
  p.wait = async (ms) => {
    for (let left = ms; left > 0; left -= 250) {
      p.t += Math.min(250, left);
      p.timers.forEach((fn) => fn && fn());
      await new Promise((r) => setImmediate(r));
    }
  };
  return p;
}

const settle = () => new Promise((r) => setImmediate(r));

async function keepAlive() {
  const p = page();
  const c = new p.ctx.AbortController();
  p.ctx.fetch('https://rr1.googlevideo.com/videoplayback?x', { method: 'POST', signal: c.signal });
  await settle();
  const d = p.downloads[0];
  p.t = 5000; d.push(1000); await settle();
  p.t = 6000; c.abort();
  assert.strictEqual(c.signal.aborted, false, 'a download still receiving data is kept');
  assert.ok(p.logs.some((l) => l.includes('kept a download')), 'keeping it is logged');

  p.t = 9000; c.abort();
  assert.strictEqual(c.signal.aborted, true, 'a download with no data for 3 s is aborted');
}

async function abortsThatGoThrough() {
  const p = page();
  const video = new p.ctx.HTMLVideoElement('v');
  // Right after a seek.
  const a = new p.ctx.AbortController();
  p.ctx.fetch(new p.ctx.Request('https://x.googlevideo.com/videoplayback', { signal: a.signal }));
  await settle();
  p.t = 1000; p.downloads[0].push(10); await settle();
  p.dispatch('seeking', video);
  p.t = 1500; a.abort();
  assert.strictEqual(a.signal.aborted, true, 'aborts right after a seek go through');
  // An old download.
  const b = new p.ctx.AbortController();
  p.ctx.fetch('https://x.googlevideo.com/videoplayback', { signal: b.signal });
  await settle();
  p.t = 70000; p.downloads[1].push(10); await settle();
  b.abort();
  assert.strictEqual(b.signal.aborted, true, 'a download over 60 s old is aborted');
  // Not a video download.
  const c = new p.ctx.AbortController();
  p.ctx.fetch('https://www.youtube.com/youtubei/v1/next', { signal: c.signal });
  c.abort();
  assert.strictEqual(c.signal.aborted, true, 'other requests are untouched');
}

async function stallGate() {
  const p = page();
  const v = new p.ctx.HTMLVideoElement('v');
  v.currentTime = 100; v.ranges = [[90, 100.2]];
  p.dispatch('waiting', v);
  assert.strictEqual(v.paused, true, 'a stall holds playback');
  assert.ok(!p.seen.some(([t]) => t === 'pause'), 'the page does not see the hold as a pause');
  v.ranges = [[90, 103]];
  await p.wait(1000);
  assert.strictEqual(v.paused, true, '3 s buffered is not enough');
  v.ranges = [[90, 105.5]];
  await p.wait(250);
  assert.strictEqual(v.paused, false, 'resumes with 5 s buffered');
  assert.ok(!p.seen.some(([t]) => t === 'play'), 'the page does not see the resume as a play');

  // Within 5 s of a seek, 2.5 s is enough.
  p.dispatch('seeking', v);
  await p.wait(1000);
  v.currentTime = 300; v.ranges = [[300, 300.1]];
  p.dispatch('waiting', v);
  v.ranges = [[300, 302.7]];
  await p.wait(250);
  assert.strictEqual(v.paused, false, 'after a seek, 2.5 s buffered resumes');
}

async function userWins() {
  const p = page();
  const v = new p.ctx.HTMLVideoElement('v');
  v.currentTime = 50; v.ranges = [[0, 50]];
  p.dispatch('waiting', v);
  v.pause();  // the user pauses during the hold
  assert.ok(p.seen.some(([t]) => t === 'pause'), 'the page sees its own pause');
  v.ranges = [[0, 70]];
  await p.wait(1000);
  assert.strictEqual(v.paused, true, 'no resume after the user paused');

  // A hold never outlasts 60 s.
  const q = page();
  const w = new q.ctx.HTMLVideoElement('w');
  w.currentTime = 10; w.ranges = [[0, 10]];
  q.dispatch('waiting', w);
  await q.wait(61000);
  assert.strictEqual(w.paused, false, 'resumes after 60 s at most');
}

// Home screen previews are off unless the add-on turns them on
// (__coretubeVideoPreviews): TizenTube's option and YouTube's flag follow it.
async function videoPreviews() {
  const flags = () => new Map([['ENABLE_PREVIEWS_WITH_SOUND', true]]);
  const config = '{"enableAdBlock":true,"enablePreviews":true}';

  const off = page({ localStorage: { 'ytaf-configuration': config } });
  assert.deepStrictEqual(JSON.parse(off.ctx.localStorage['ytaf-configuration']),
    { enableAdBlock: true, enablePreviews: false }, 'off by default; other options kept');
  const yt = flags();
  off.ctx._yttv = { a: {}, b: yt };  // YouTube's settings appear later
  off.runTimeouts();
  assert.strictEqual(yt.get('ENABLE_PREVIEWS_WITH_SOUND'), false, "YouTube's flag off");

  const on = page({ __coretubeVideoPreviews: true, _yttv: { b: flags() } });
  assert.strictEqual(JSON.parse(on.ctx.localStorage['ytaf-configuration']).enablePreviews, true,
    'on when the add-on turns them on, with no stored options yet');
  assert.strictEqual(on.ctx._yttv.b.get('ENABLE_PREVIEWS_WITH_SOUND'), true);

  const broken = page({ localStorage: { 'ytaf-configuration': '{oops' } });
  assert.strictEqual(broken.ctx.localStorage['ytaf-configuration'], '{oops', 'unreadable options left alone');
  assert.ok(broken.logs.some((l) => l.includes('cannot set the video previews option')));

  const never = page();  // YouTube's settings never appear: stop looking after a minute
  never.t = 61000;
  never.runTimeouts();
  assert.strictEqual(never.timeouts.length, 0);
}

// The keep-alive and the stall gate act only on the watch page, never on a preview,
// and never resume a video the page removed.
async function watchPageOnly() {
  const p = page();
  p.ctx.location.hash = '#/';
  const c = new p.ctx.AbortController();
  p.ctx.fetch('https://rr1.googlevideo.com/videoplayback?x', { signal: c.signal });
  await settle();
  p.t = 1000; p.downloads[0].push(10); await settle();
  c.abort();
  assert.strictEqual(c.signal.aborted, true, 'a preview download is aborted when the page asks');
  const v = new p.ctx.HTMLVideoElement('v');
  v.currentTime = 1; v.ranges = [[0, 1.1]];
  p.dispatch('waiting', v);
  assert.strictEqual(v.paused, false, 'a preview is never held');

  const q = page();
  const w = new q.ctx.HTMLVideoElement('w');
  w.currentTime = 10; w.ranges = [[0, 10.1]];
  q.dispatch('waiting', w);
  assert.strictEqual(w.paused, true);
  w.isConnected = false;  // the page removed it
  w.ranges = [[0, 30]];
  await q.wait(500);
  assert.strictEqual(w.paused, true, 'a removed video is not resumed');
  const x = new q.ctx.HTMLVideoElement('x');
  x.currentTime = 5; x.ranges = [[0, 5.1]];
  q.dispatch('waiting', x);
  assert.strictEqual(x.paused, true, 'the next video can be held again');
}

async function onlyOnYouTube() {
  const ctx = { location: { hostname: 'h5vcc-embedded.example' }, console: { info() {} } };
  ctx.window = ctx;
  vm.runInNewContext(source, ctx);
  assert.strictEqual(ctx.__coretubeSabr, undefined, 'other pages are left alone');
}

(async () => {
  for (const t of [keepAlive, abortsThatGoThrough, stallGate, userWins, videoPreviews, watchPageOnly, onlyOnYouTube]) {
    await t();
    console.log(t.name + ': ok');
  }
})().catch((e) => { console.error(e); process.exit(1); });
