const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { command } = require('./redis');
const prefix = 'product-demo:product:';
const samples = [
  { id: '1001', name: 'Studio Headphones', category: 'AUDIO', price: 129000, kind: 'headphones', color: 'sand' },
  { id: '1002', name: 'Everyday Keyboard', category: 'DESK', price: 89000, kind: 'keyboard', color: 'mint' },
  { id: '1003', name: 'Pocket Speaker', category: 'AUDIO', price: 59000, kind: 'speaker', color: 'lavender' },
  { id: '1004', name: 'Arc Desk Lamp', category: 'LIVING', price: 45000, kind: 'lamp', color: 'peach' }
];
function createApp(config = {}) {
  const stats = { hits: 0, misses: 0, requests: 0, totalMs: 0 };
  const logs = [];
  const run = args => command(args, config);
  const record = (cmd, result, ms) => logs.unshift({ time: Date.now(), command: cmd, result, ms: +ms.toFixed(2) });
  function json(res, status, data) {
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(data));
  }
  async function body(req) {
    let input = '';
    for await (const chunk of req) {
      input += chunk;
      if (Buffer.byteLength(input) > 8192) throw Object.assign(Error('입력 크기 초과'), { status: 413 });
    }
    try { return JSON.parse(input || '{}'); } catch { throw Object.assign(Error('JSON 형식 오류'), { status: 400 }); }
  }
  function validate(value) {
    if (!/^[a-zA-Z0-9_-]{1,40}$/.test(value.id || '') || typeof value.name !== 'string' || !value.name.trim() || value.name.length > 80 || !Number.isSafeInteger(value.price) || value.price < 0 || !Number.isInteger(value.ttl) || value.ttl < 1 || value.ttl > 300) {
      throw Object.assign(Error('ID, 상품명, 0 이상의 정수 가격, TTL 1~300초를 확인하세요.'), { status: 400 });
    }
  }
  async function save(product, ttl) {
    const start = performance.now();
    await run(['SET', prefix + product.id, JSON.stringify(product), 'EX', ttl]);
    record(`SET ${prefix + product.id} <상품 JSON> EX ${ttl}`, 'OK', performance.now() - start);
  }
  return http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    try {
      // Only same-origin browser writes are accepted. No arbitrary Redis commands are exposed.
      if (['POST', 'DELETE'].includes(req.method) && req.headers.origin && req.headers.origin !== `http://${req.headers.host}`) return json(res, 403, { error: '다른 출처의 요청은 허용하지 않습니다.' });
      if (url.pathname === '/api/state' && req.method === 'GET') {
        await run(['PING']);
        const keys = (await run(['KEYS', '*'])).filter(key => key.startsWith(prefix));
        const products = (await Promise.all(keys.map(async key => {
          const raw = await run(['GET', key]);
          const ttl = await run(['TTL', key]);
          if (raw === null || ttl === -2) return null;
          try { return { ...JSON.parse(raw), key, ttl }; } catch { return null; }
        }))).filter(Boolean).sort((a,b) => a.id.localeCompare(b.id));
        return json(res, 200, { connected: true, products, stats, logs: logs.slice(0, 80), dbms: null });
      }
      if (url.pathname === '/api/seed' && req.method === 'POST') {
        const { ttl = 120 } = await body(req);
        if (!Number.isInteger(ttl) || ttl < 1 || ttl > 300) return json(res, 400, { error: 'TTL은 1~300초입니다.' });
        for (const product of samples) await save(product, ttl);
        logs.splice(80);
        return json(res, 200, { saved: samples.length });
      }
      const match = url.pathname.match(/^\/api\/products\/([a-zA-Z0-9_-]{1,40})$/);
      if (match) {
        const key = prefix + match[1];
        if (req.method === 'GET') {
          const start = performance.now();
          const raw = await run(['GET', key]);
          const ms = performance.now() - start;
          const hit = raw !== null;
          stats.requests++; stats[hit ? 'hits' : 'misses']++; stats.totalMs += ms;
          record(`GET ${key}`, hit ? 'HIT' : 'MISS', ms); logs.splice(80);
          return json(res, 200, { hit, ms: +ms.toFixed(2), product: hit ? JSON.parse(raw) : null, key, dbms: null });
        }
        if (req.method === 'POST') {
          const value = { ...await body(req), id: match[1] }; validate(value);
          const { ttl, name, price, id } = value;
          await save({ id, name: name.trim(), price, category: 'CUSTOM', kind: 'box', color: 'sand' }, ttl);
          logs.splice(80); return json(res, 200, { saved: true });
        }
        if (req.method === 'DELETE') {
          const start = performance.now(); const deleted = await run(['DEL', key]);
          record(`DEL ${key}`, String(deleted), performance.now() - start); logs.splice(80);
          return json(res, 200, { deleted });
        }
      }
      const file = { '/': 'index.html', '/app.js': 'app.js', '/style.css': 'style.css' }[url.pathname];
      if (!file || req.method !== 'GET') return json(res, 404, { error: '찾을 수 없습니다.' });
      const data = await fs.readFile(path.join(__dirname, 'public', file));
      res.writeHead(200, { 'Content-Type': file.endsWith('.html') ? 'text/html; charset=utf-8' : file.endsWith('.css') ? 'text/css' : 'text/javascript', 'Cache-Control': 'no-store' }); res.end(data);
    } catch (error) { json(res, error.status || 503, { error: error.status ? error.message : `Redis 요청 실패: ${error.message}`, connected: false }); }
  });
}
if (require.main === module) createApp({ host: process.env.REDIS_HOST, port: Number(process.env.REDIS_PORT || 16380) }).listen(Number(process.env.WEB_PORT || 3000), '127.0.0.1', () => console.log(`상품 조회: http://127.0.0.1:${process.env.WEB_PORT || 3000}`));
module.exports = { createApp };
