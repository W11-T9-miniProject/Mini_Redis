const { spawn } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
const { command } = require('./redis');
const { createApp } = require('./server');
const port = Number(process.env.REDIS_PORT || 16380);
const binary = path.resolve(__dirname, '../c-mini-redis/mini-redis');
if (!fs.existsSync(binary)) { console.error('C Mini Redis를 먼저 빌드하세요: cd ../c-mini-redis && make'); process.exit(1); }
const data = path.join(__dirname, '.redis-data'); fs.mkdirSync(data, { recursive: true });
const redis = spawn(binary, ['--bind', '127.0.0.1', '--port', String(port), '--dir', data], { stdio: 'inherit' });
let app, stopping = false;
function stop(code = 0) {
  if (stopping) return; stopping = true;
  app?.close(); redis.kill('SIGTERM');
  setTimeout(() => process.exit(code), 1500).unref();
  redis.once('exit', () => process.exit(code));
}
redis.on('error', error => { console.error(error.message); stop(1); });
redis.on('exit', code => { if (!stopping) { console.error(`Redis 종료 (${code})`); app?.close(); process.exit(code || 1); } });
process.on('SIGINT', () => stop()); process.on('SIGTERM', () => stop());
(async () => {
  for (let i = 0; i < 50 && !stopping; i++) {
    try {
      await command(['PING'], { port });
      if (stopping) return;
      app = createApp({ port });
      app.on('error', error => { console.error(error.message); stop(1); });
      app.listen(Number(process.env.WEB_PORT || 3000), '127.0.0.1', () => console.log(`상품 조회: http://127.0.0.1:${process.env.WEB_PORT || 3000}`));
      return;
    } catch { await new Promise(resolve => setTimeout(resolve, 100)); }
  }
  if (!stopping) { console.error('Redis 시작 실패'); stop(1); }
})();
