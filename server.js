#!/usr/bin/env node
'use strict';

const net = require('node:net');

class MiniRedis {
  constructor() { this.data = new Map(); }
  entry(key) {
    const item = this.data.get(key);
    if (item && item.expiresAt !== null && item.expiresAt <= Date.now()) {
      this.data.delete(key);
      return null;
    }
    return item || null;
  }
  execute(args) {
    if (!args.length) return ['error', 'empty command'];
    const [command, ...rest] = args;
    const cmd = command.toUpperCase();
    if (cmd === 'PING') return rest.length ? ['bulk', rest.join(' ')] : ['simple', 'PONG'];
    if (cmd === 'SET') {
      if (rest.length < 2) return ['error', 'wrong number of arguments for SET'];
      const [key, value, ...options] = rest;
      let expiresAt = null;
      if (options.length) {
        if (options.length !== 2 || !['EX', 'PX'].includes(options[0].toUpperCase())) return ['error', 'syntax error'];
        const amount = Number(options[1]);
        if (!Number.isSafeInteger(amount) || amount <= 0) return ['error', 'invalid expire time'];
        expiresAt = Date.now() + amount * (options[0].toUpperCase() === 'EX' ? 1000 : 1);
      }
      this.data.set(key, { value, expiresAt });
      return ['simple', 'OK'];
    }
    if (cmd === 'GET') {
      if (rest.length !== 1) return ['error', 'wrong number of arguments for GET'];
      const item = this.entry(rest[0]); return item ? ['bulk', item.value] : ['null'];
    }
    if (cmd === 'DEL' || cmd === 'EXISTS') {
      if (!rest.length) return ['error', `wrong number of arguments for ${cmd}`];
      let count = 0;
      for (const key of rest) {
        const exists = !!this.entry(key);
        if (exists) { count++; if (cmd === 'DEL') this.data.delete(key); }
      }
      return ['integer', count];
    }
    if (cmd === 'EXPIRE') {
      if (rest.length !== 2) return ['error', 'wrong number of arguments for EXPIRE'];
      const seconds = Number(rest[1]);
      if (!Number.isSafeInteger(seconds)) return ['error', 'value is not an integer or out of range'];
      const item = this.entry(rest[0]);
      if (!item) return ['integer', 0];
      if (seconds <= 0) this.data.delete(rest[0]); else item.expiresAt = Date.now() + seconds * 1000;
      return ['integer', 1];
    }
    if (cmd === 'TTL') {
      if (rest.length !== 1) return ['error', 'wrong number of arguments for TTL'];
      const item = this.entry(rest[0]);
      return ['integer', !item ? -2 : item.expiresAt === null ? -1 : Math.max(0, Math.ceil((item.expiresAt - Date.now()) / 1000))];
    }
    return ['error', `unknown command '${command}'`];
  }
}

function parse(buffer) {
  const crlf = (from) => buffer.indexOf('\r\n', from);
  const line = (from) => { const end = crlf(from); return end < 0 ? null : { value: buffer.toString('utf8', from, end), next: end + 2 }; };
  if (!buffer.length) return null;
  if (buffer[0] !== 42) {
    const first = line(0); if (!first) return null;
    const args = first.value.trim().split(/\s+/);
    return { args, next: first.next };
  }
  const header = line(1); if (!header) return null;
  const count = Number(header.value); if (!Number.isInteger(count) || count < 0) return { error: true, next: header.next };
  let cursor = header.next; const args = [];
  for (let i = 0; i < count; i++) {
    if (buffer[cursor] !== 36) return { error: true, next: cursor + 1 };
    const sizeLine = line(cursor + 1); if (!sizeLine) return null;
    const size = Number(sizeLine.value); if (!Number.isInteger(size) || size < 0) return { error: true, next: sizeLine.next };
    const end = sizeLine.next + size;
    if (buffer.length < end + 2) return null;
    if (buffer[end] !== 13 || buffer[end + 1] !== 10) return { error: true, next: end + 2 };
    args.push(buffer.toString('utf8', sizeLine.next, end)); cursor = end + 2;
  }
  return { args, next: cursor };
}

function encode(reply) {
  switch (reply[0]) {
    case 'simple': return `+${reply[1]}\r\n`;
    case 'error': return `-ERR ${reply[1]}\r\n`;
    case 'integer': return `:${reply[1]}\r\n`;
    case 'bulk': return `$${Buffer.byteLength(reply[1])}\r\n${reply[1]}\r\n`;
    default: return '$-1\r\n';
  }
}

const store = new MiniRedis();
const host = process.env.HOST || '127.0.0.1';
const port = Number(process.env.PORT || 6379);
const server = net.createServer((socket) => {
  let buffer = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length) {
      const parsed = parse(buffer);
      if (!parsed) break;
      buffer = buffer.subarray(parsed.next);
      if (parsed.error) { socket.write('-ERR Protocol error\r\n'); continue; }
      const reply = store.execute(parsed.args);
      socket.write(encode(reply));
    }
  });
});
server.listen(port, host, () => console.log(`mini-redis listening on ${host}:${port}`));
server.on('error', (err) => { console.error(err.message); process.exitCode = 1; });
