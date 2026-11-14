const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { mkdtemp, rm } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const path = require('node:path');
const net = require('node:net');
const { once } = require('node:events');
const { createApp } = require('./server');
const { command, parse } = require('./redis');
test('RESP handles split UTF-8 bulk and arrays', () => {
  const wire = Buffer.from('$6\r\n한글\r\n');
  assert.equal(parse(wire.subarray(0, 7)), null);
  assert.equal(parse(wire).value, '한글');
  assert.deepEqual(parse(Buffer.from('*2\r\n$1\r\na\r\n:3\r\n')).value, ['a', 3]);
});
test('real C Redis: product lifecycle, TTL, isolation, validation and outage', async () => {
  const probe = net.createServer(); probe.listen(0, '127.0.0.1'); await once(probe, 'listening');
  const port = probe.address().port; await new Promise(resolve => probe.close(resolve));
  const directory = await mkdtemp(path.join(tmpdir(), 'redis-market-'));
  const child = spawn(path.resolve(__dirname, '../c-mini-redis/mini-redis'), ['--bind','127.0.0.1','--port',String(port),'--dir',directory]);
  let app;
  try {
    for (let i = 0; ; i++) {
      try { await command(['PING'], {port}); break; } catch (error) { if(i > 40) throw error; await new Promise(r => setTimeout(r, 50)); }
    }
    app = createApp({port}); app.listen(0,'127.0.0.1'); await once(app, 'listening');
    const base = `http://127.0.0.1:${app.address().port}`;
    async function request(route, method = 'GET', value) {
      const response = await fetch(base + route, {method, headers: {'Content-Type':'application/json'}, body: value === undefined ? undefined : JSON.stringify(value)});
      return { status: response.status, ...await response.json() };
    }
    assert.equal((await fetch(base)).status, 200);
    let state = await request('/api/state'); assert.equal(state.dbms, null); assert.equal(state.products.length, 0);
    assert.equal((await request('/api/seed','POST',{ttl:60})).saved, 4);
    assert.equal((await request('/api/products/1001')).hit, true);
    assert.equal((await request('/api/products/unknown')).hit, false);
    assert.equal((await request('/api/products/custom','POST',{name:'한글 상품 <script>',price:12300,ttl:1})).saved, true);
    assert.equal((await request('/api/products/custom')).product.name, '한글 상품 <script>');
    await command(['SET','unrelated','keep','EX',60],{port});
    state = await request('/api/state'); assert.equal(state.products.length,5); assert.equal(state.stats.requests,3);
    assert.ok(state.products.find(p => p.id === 'custom').ttl >= 0);
    assert.equal((await request('/api/products/custom','POST',{name:'bad',price:-1,ttl:500})).status,400);
    await new Promise(r => setTimeout(r,1200));
    assert.equal((await request('/api/products/custom')).hit,false);
    assert.equal((await request('/api/products/1001','DELETE')).deleted,1);
    assert.equal((await request('/api/products/1001')).hit,false);
    assert.equal(await command(['GET','unrelated'],{port}),'keep');
    state = await request('/api/state'); assert.equal(state.stats.hits,2); assert.equal(state.stats.misses,3); assert.equal(state.products.length,3);
    const blocked = await fetch(base+'/api/seed',{method:'POST',headers:{Origin:'http://other.example'}}); assert.equal(blocked.status,403);
    child.kill('SIGTERM'); await once(child,'exit');
    assert.equal((await request('/api/state')).status,503);
  } finally {
    if(app) { app.closeAllConnections(); await new Promise(resolve => app.close(resolve)); }
    if(child.exitCode === null && child.signalCode === null) { child.kill('SIGTERM'); await once(child,'exit'); }
    await rm(directory,{recursive:true,force:true});
  }
});
