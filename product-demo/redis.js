const net = require('node:net');
function parse(buffer, offset = 0) {
  const end = buffer.indexOf('\r\n', offset);
  if (end < 0) return null;
  const type = String.fromCharCode(buffer[offset]);
  const line = buffer.toString('utf8', offset + 1, end);
  let next = end + 2;
  if (type === '+' || type === '-') return { value: line, error: type === '-', next };
  if (type === ':') return { value: Number(line), next };
  const length = Number(line);
  if (length === -1) return { value: null, next };
  if (type === '$') {
    if (buffer.length < next + length + 2) return null;
    return { value: buffer.toString('utf8', next, next + length), next: next + length + 2 };
  }
  if (type === '*') {
    const value = [];
    for (let i = 0; i < length; i++) {
      const item = parse(buffer, next);
      if (!item) return null;
      if (item.error) throw Error(item.value);
      value.push(item.value); next = item.next;
    }
    return { value, next };
  }
  throw Error('지원하지 않는 RESP 응답');
}
function command(args, config = {}) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: config.host || '127.0.0.1', port: config.port || 16380 });
    let buffer = Buffer.alloc(0);
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true; socket.destroy(); error ? reject(error) : resolve(value);
    };
    socket.setTimeout(2000, () => finish(Error('Redis 응답 시간 초과')));
    socket.on('error', error => finish(error));
    socket.on('end', () => finish(Error('Redis 연결 종료')));
    socket.on('connect', () => {
      const parts = [Buffer.from(`*${args.length}\r\n`)];
      for (const arg of args) {
        const value = Buffer.from(String(arg));
        parts.push(Buffer.from(`$${value.length}\r\n`), value, Buffer.from('\r\n'));
      }
      socket.write(Buffer.concat(parts));
    });
    socket.on('data', chunk => {
      buffer = Buffer.concat([buffer, chunk]);
      try {
        const reply = parse(buffer);
        if (reply) finish(reply.error ? Error(reply.value) : null, reply.value);
      } catch (error) { finish(error); }
    });
  });
}
module.exports = { command, parse };
