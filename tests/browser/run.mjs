// The web plugin in a real browser: each case starts a Lube app (on a free port), opens it in headless Chrome,
// runs its steps in the page, and compares what they saw (and the page's console) with its .out file.
//
//   node tests/browser/run.mjs [path-to-lube] [--update]     (make browser; needs Node 22+ and Chrome or Chromium)
//
// CHROME=/path/to/chrome picks the browser. Without one it says so and passes (it is not part of make test),
// unless it runs with --require, as CI does.
// The steps run with until(test, what) and settled() (no event waits to go or for its answer: data-busy is gone).
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
process.chdir(join(here, '..', '..'));  // (Lube's folder: the apps name their files from there)
const update = process.argv.includes('--update'), required = process.argv.includes('--require');
const lube = process.argv.slice(2).find(a => !a.startsWith('--')) ?? './lube';

const CASES = [
  { name: 'todo', app: 'examples/web/todo.lube' },  // the example app
  { name: 'keys', app: 'tests/browser/keys.lube', stop: '/stop' },  // lists that move, fields, errors
];

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const known = ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
                 '/Applications/Chromium.app/Contents/MacOS/Chromium',
                 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
                 'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe'];
  for (const p of known) if (existsSync(p)) return p;
  for (const name of ['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser'])
    for (const dir of (process.env.PATH ?? '').split(delimiter))
      if (existsSync(join(dir, name))) return join(dir, name);
  return null;
}

// Waits for a line of out that matches re (a process's stderr or stdout), or fails after ms.
function waitFor(proc, re, ms, what) {
  return new Promise((ok, fail) => {
    let seen = '';
    const look = d => {
      seen += d;
      const m = seen.match(re);
      if (m) { clearTimeout(timer); ok(m[1]); }
    };
    proc.stdout?.on('data', look);
    proc.stderr?.on('data', look);
    proc.on('error', e => fail(new Error(`${what} could not start: ${e.message}`)));
    proc.on('exit', code => fail(new Error(`${what} ended (${code}) before it was ready:\n${seen}`)));
    const timer = setTimeout(() => fail(new Error(`${what} was not ready in ${ms / 1000} s:\n${seen}`)), ms);
  });
}

// One page in the browser, over the DevTools protocol.
async function openPage(devPort) {
  const target = await (await fetch(`http://127.0.0.1:${devPort}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, fail) => { ws.onopen = ok; ws.onerror = fail; });
  let id = 0;
  const waiting = new Map(), events = new Map(), console = [];
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
    if (m.method && events.has(m.method)) { events.get(m.method)(m.params); events.delete(m.method); }
    if (m.method === 'Runtime.consoleAPICalled')
      console.push(m.params.type + ': ' + m.params.args.map(a => a.value ?? a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown')
      console.push('exception: ' + (m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text));
    if (m.method === 'Log.entryAdded' && !/\/favicon\.ico$/.test(m.params.entry.url ?? ''))  // (no app has one)
      console.push('log: ' + m.params.entry.level + ' ' + m.params.entry.text);
  };
  const call = (method, params = {}) => new Promise(ok => {
    const n = ++id;
    waiting.set(n, ok);
    ws.send(JSON.stringify({ id: n, method, params }));
  });
  const next = method => new Promise(ok => events.set(method, ok));
  await call('Runtime.enable');
  await call('Log.enable');
  await call('Page.enable');
  return {
    console,
    async go(url) {
      const loaded = next('Page.loadEventFired');
      await call('Page.navigate', { url });
      await loaded;
    },
    async run(source) {  // (with the helpers, in a scope of its own)
      const expression = `(() => {${HELPERS}\nreturn (\n${source}\n);\n})()`;
      const r = await call('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      if (r.result.exceptionDetails) return { error: r.result.exceptionDetails.exception?.description ?? r.result.exceptionDetails.text };
      return r.result.result.value;
    },
    close() { ws.close(); fetch(`http://127.0.0.1:${devPort}/json/close/${target.id}`).catch(() => {}); },
  };
}

// What every case's steps have: until(test, what, ms) waits for test() to be true; settled() for the page to
// have the server's last answer.
const HELPERS = `
const wait = ms => new Promise(r => setTimeout(r, ms));
const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];
async function until(test, what, ms = 5000) {
  for (const end = Date.now() + ms; !test(); await wait(10))
    if (Date.now() > end) throw new Error('waited ' + ms / 1000 + ' s for ' + what);
}
const settled = () => until(() => !document.getElementById('lube').hasAttribute('data-busy'), 'the server to answer');
`;

const chromePath = findChrome();
if (!chromePath) {
  console.log('browser tests: skipped (no Chrome or Chromium found; CHROME=/path/to/chrome picks one)');
  process.exit(required ? 1 : 0);
}
if (typeof WebSocket === 'undefined') {
  console.log('browser tests: skipped (this Node has no WebSocket: it needs Node 22 or later)');
  process.exit(required ? 1 : 0);
}

const profile = mkdtempSync(join(tmpdir(), 'lube-chrome-'));
const chrome = spawn(chromePath, ['--headless=new', '--remote-debugging-port=0', '--no-first-run',
  '--no-default-browser-check', '--disable-extensions', `--user-data-dir=${profile}`, 'about:blank',
  ...(process.env.CI ? ['--no-sandbox'] : [])],  // (CI's Linux may not allow Chrome's sandbox; these pages are ours)
  { stdio: ['ignore', 'ignore', 'pipe'] });
let pass = 0, fail = 0;
try {
  const devPort = new URL(await waitFor(chrome, /DevTools listening on (ws:\/\/\S+)/, 20000, 'Chrome')).port;
  for (const c of CASES) {
    const server = spawn(lube, [c.app, '0'], { stdio: ['ignore', 'pipe', 'pipe'] });
    let serverOut = '';
    server.stdout.on('data', d => serverOut += d);
    server.stderr.on('data', d => serverOut += d);
    let page = null;
    try {
      const url = await waitFor(server, /serving at (http:\S+)/, 10000, c.app);
      page = await openPage(devPort);
      await page.go(url);
      const seen = await page.run(readFileSync(join(here, c.name + '.js'), 'utf8'));
      // The server goes: the page says so (data-closed), and can be styled for it. An app with a stop route
      // ends as a program does (and must end well: lube-check fails at exit on a leak); the others are killed.
      const ended = new Promise(ok => server.on('exit', (code, signal) => ok(signal ?? code)));
      if (c.stop) await fetch(new URL(c.stop, url));
      else server.kill();
      const how = await ended;
      if (c.stop && how !== 0) throw new Error(`${c.app} ended with ${how}:\n${serverOut}`);
      const closed = await page.run(`until(() => $('#lube').hasAttribute('data-closed'), 'data-closed', 3000).then(() => true, () => false)`);
      const got = JSON.stringify({ steps: seen, closedWhenServerStops: closed, console: page.console }, null, 1) + '\n';
      const outFile = join(here, c.name + '.out');
      if (update) writeFileSync(outFile, got);
      const want = existsSync(outFile) ? readFileSync(outFile, 'utf8') : '';
      if (got === want) pass++;
      else {
        fail++;
        console.log(`FAIL ${c.name}\n--- expected (${outFile})\n${want}--- got\n${got}--- the app said\n${serverOut}`);
      }
    } catch (e) {
      fail++;
      console.log(`FAIL ${c.name}: ${e.message}`);
    } finally {
      page?.close();
      if (server.exitCode === null) server.kill();
    }
  }
} catch (e) {
  fail++;
  console.log('FAIL ' + e.message);
} finally {
  chrome.kill();
  if (chrome.pid) await new Promise(ok => chrome.exitCode !== null ? ok() : chrome.on('exit', ok));
  try {  // (Chrome's last processes may still be writing there: tried again, and not a failure if it stays)
    rmSync(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  } catch (e) {
    console.log(`(could not remove ${profile}: ${e.code})`);
  }
}
console.log(`browser tests: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
