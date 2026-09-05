'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const repositoryRoot = path.resolve(__dirname, '..');
const htmlPath = path.join(repositoryRoot, 'main', 'www', 'index.html');

function shippedScript() {
  const html = fs.readFileSync(htmlPath, 'utf8');
  const match = html.match(/<script>([\s\S]*?)<\/script>/);
  assert.ok(match, 'embedded WebUI script not found');
  let script = match[1];

  switch (process.env.WEBUI_TEST_MUTATION) {
    case undefined:
      break;
    case 'missing-startup':
      script = script.replace(/\npollDashboard\(\);\s*$/, '\n');
      break;
    case 'parallel-polling':
      script = script.replace(
        '    await refresh();\n    await refreshWifi();\n    await refreshOta();',
        '    refresh();\n    refreshWifi();\n    refreshOta();'
      );
      break;
    case 'missing-timeout':
      script = script.replace(
        '  const timer = setTimeout(() => controller.abort(), POLL_REQUEST_TIMEOUT_MS);',
        '  const timer = 0;'
      );
      break;
    case 'missing-softap-guard':
      script = script.replace(
        "$('otaform').addEventListener('submit', async ev => {\n  ev.preventDefault();\n  if (softApControlPlaneActive) {",
        "$('otaform').addEventListener('submit', async ev => {\n  ev.preventDefault();\n  if (false) {"
      );
      break;
    default:
      throw new Error(`unknown mutation: ${process.env.WEBUI_TEST_MUTATION}`);
  }
  return script;
}

class FakeElement {
  constructor() {
    this.checked = false;
    this.disabled = false;
    this.innerHTML = '';
    this.placeholder = '';
    this.scrollHeight = 0;
    this.scrollTop = 0;
    this.style = {};
    this.textContent = '';
    this.title = '';
    this.value = '';
    this.listeners = new Map();
  }

  addEventListener(type, listener) {
    this.listeners.set(type, listener);
  }

  async dispatch(type) {
    const listener = this.listeners.get(type);
    assert.ok(listener, `missing ${type} listener`);
    await listener({preventDefault() {}});
  }
}

class FakeClock {
  constructor() {
    this.now = 0;
    this.nextId = 1;
    this.timers = new Map();
  }

  setTimeout(callback, delay) {
    const id = this.nextId++;
    this.timers.set(id, {callback, due: this.now + Number(delay), delay: Number(delay)});
    return id;
  }

  clearTimeout(id) {
    this.timers.delete(id);
  }

  delays() {
    return [...this.timers.values()].map(timer => timer.delay).sort((a, b) => a - b);
  }

  async advance(milliseconds) {
    const target = this.now + milliseconds;
    while (true) {
      const due = [...this.timers.entries()]
        .filter(([, timer]) => timer.due <= target)
        .sort((left, right) => left[1].due - right[1].due || left[0] - right[0])[0];
      if (!due) break;
      const [id, timer] = due;
      this.timers.delete(id);
      this.now = timer.due;
      timer.callback();
      await drainMicrotasks();
    }
    this.now = target;
    await drainMicrotasks();
  }
}

class DeferredFetch {
  constructor(url, options) {
    this.url = url;
    this.options = options;
    this.settled = false;
    this.promise = new Promise((resolve, reject) => {
      this.resolvePromise = resolve;
      this.rejectPromise = reject;
    });
    if (options.signal) {
      options.signal.addEventListener('abort', () => {
        const error = new Error('aborted');
        error.name = 'AbortError';
        this.reject(error);
      }, {once: true});
    }
  }

  resolve(payload, overrides = {}) {
    if (this.settled) return;
    this.settled = true;
    this.resolvePromise({
      ok: overrides.ok === undefined ? true : overrides.ok,
      status: overrides.status === undefined ? 200 : overrides.status,
      async json() { return payload; },
      async text() { return overrides.text || ''; },
    });
  }

  reject(error) {
    if (this.settled) return;
    this.settled = true;
    this.rejectPromise(error);
  }
}

class BrowserHarness {
  constructor() {
    this.clock = new FakeClock();
    this.elements = new Map();
    this.calls = [];
    const document = {
      activeElement: null,
      getElementById: id => {
        if (!this.elements.has(id)) this.elements.set(id, new FakeElement());
        return this.elements.get(id);
      },
    };
    const fetch = (url, options = {}) => {
      const call = new DeferredFetch(url, options);
      this.calls.push(call);
      if (url === '/api/mqtt') {
        call.resolve({});
      } else if (url === '/api/ota/check' && options.method === 'POST') {
        call.resolve({});
      }
      return call.promise;
    };
    this.context = vm.createContext({
      AbortController,
      console,
      confirm: () => true,
      document,
      fetch,
      setTimeout: this.clock.setTimeout.bind(this.clock),
      clearTimeout: this.clock.clearTimeout.bind(this.clock),
    });
    vm.runInContext(shippedScript(), this.context, {filename: htmlPath});
  }

  callsFor(url, method = undefined) {
    return this.calls.filter(call =>
      call.url === url && (method === undefined || call.options.method === method)
    );
  }

  pending(url) {
    return this.callsFor(url).find(call => !call.settled);
  }

  async resolve(url, payload = payloadFor(url)) {
    const call = this.pending(url);
    assert.ok(call, `no pending fetch for ${url}`);
    call.resolve(payload);
    await drainMicrotasks();
  }

  async reject(url, message = 'network failed') {
    const call = this.pending(url);
    assert.ok(call, `no pending fetch for ${url}`);
    call.reject(new Error(message));
    await drainMicrotasks();
  }

  evaluate(expression) {
    return vm.runInContext(expression, this.context);
  }

  element(id) {
    return this.elements.get(id) || this.context.document.getElementById(id);
  }
}

function payloadFor(url) {
  if (url === '/api/status') return {};
  if (url === '/api/wifi') {
    return {state: 'connected', connected: true, ssid: 'test', ip: '192.0.2.1'};
  }
  if (url === '/api/ota') return {state: 'idle'};
  throw new Error(`no default payload for ${url}`);
}

async function drainMicrotasks() {
  for (let index = 0; index < 12; index++) await Promise.resolve();
}

async function finishCycle(harness, wifi = payloadFor('/api/wifi')) {
  await harness.resolve('/api/status');
  await harness.resolve('/api/wifi', wifi);
  await harness.resolve('/api/ota');
}

const tests = [
  ['script load starts dashboard polling', async () => {
    const harness = new BrowserHarness();
    await drainMicrotasks();
    assert.equal(harness.callsFor('/api/status').length, 1);
    assert.equal(harness.callsFor('/api/mqtt').length, 1);
  }],

  ['dashboard requests are sequential and MQTT config is separate', async () => {
    const harness = new BrowserHarness();
    assert.equal(harness.callsFor('/api/status').length, 1);
    assert.equal(harness.callsFor('/api/wifi').length, 0);
    assert.equal(harness.callsFor('/api/ota').length, 0);
    await harness.resolve('/api/status');
    assert.equal(harness.callsFor('/api/wifi').length, 1);
    assert.equal(harness.callsFor('/api/ota').length, 0);
    await harness.resolve('/api/wifi');
    assert.equal(harness.callsFor('/api/ota').length, 1);
    await harness.resolve('/api/ota');
    assert.equal(harness.callsFor('/api/mqtt').length, 1);
  }],

  ['slow request does not overlap dashboard cycles', async () => {
    const harness = new BrowserHarness();
    await harness.clock.advance(2000);
    assert.equal(harness.callsFor('/api/status').length, 1);
    assert.equal(harness.callsFor('/api/wifi').length, 0);
    assert.equal(harness.callsFor('/api/ota').length, 0);
  }],

  ['deadline abort clears state and later polling recovers', async () => {
    const harness = new BrowserHarness();
    await harness.clock.advance(2500);
    assert.equal(harness.evaluate('statusRefreshActive'), false);
    assert.equal(harness.callsFor('/api/wifi').length, 1);
    await harness.resolve('/api/wifi');
    await harness.resolve('/api/ota');
    await harness.clock.advance(2000);
    assert.equal(harness.callsFor('/api/status').length, 2);
    await harness.resolve('/api/status');
    assert.equal(harness.callsFor('/api/wifi').length, 2);
  }],

  ['rejected fetch allows the next dashboard cycle', async () => {
    const harness = new BrowserHarness();
    await harness.reject('/api/status');
    assert.equal(harness.evaluate('statusRefreshActive'), false);
    await harness.resolve('/api/wifi');
    await harness.resolve('/api/ota');
    await harness.clock.advance(2000);
    assert.equal(harness.callsFor('/api/status').length, 2);
  }],

  ['next cycle is delayed until after completion', async () => {
    const harness = new BrowserHarness();
    await finishCycle(harness);
    assert.deepEqual(harness.clock.delays(), [2000]);
    await harness.clock.advance(1999);
    assert.equal(harness.callsFor('/api/status').length, 1);
    await harness.clock.advance(1);
    assert.equal(harness.callsFor('/api/status').length, 2);
  }],

  ['SoftAP blocks manual OTA while STA permits it and restores submit state', async () => {
    const softApHarness = new BrowserHarness();
    await finishCycle(softApHarness, {state: 'softap', ap_ssid: 'test-ap'});
    await softApHarness.element('otaform').dispatch('submit');
    assert.equal(softApHarness.callsFor('/api/ota/check', 'POST').length, 0);

    const staHarness = new BrowserHarness();
    await finishCycle(staHarness);
    await staHarness.element('otaform').dispatch('submit');
    await drainMicrotasks();
    assert.equal(staHarness.callsFor('/api/ota/check', 'POST').length, 1);
    assert.equal(staHarness.evaluate('otaSubmitActive'), false);
    assert.equal(staHarness.element('otabtn').disabled, false);
  }],
];

(async () => {
  let failures = 0;
  for (const [name, test] of tests) {
    try {
      await test();
      process.stdout.write(`ok - ${name}\n`);
    } catch (error) {
      failures++;
      process.stderr.write(`not ok - ${name}\n${error.stack}\n`);
    }
  }
  if (failures) process.exitCode = 1;
})();
