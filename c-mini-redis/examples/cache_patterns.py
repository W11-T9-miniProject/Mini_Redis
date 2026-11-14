#!/usr/bin/env python3
"""External cache-aside/write-back and token-lock examples; standard library only."""
import argparse
from collections import OrderedDict
import json
import secrets
import socket
import sys
import threading
import time


class RedisError(RuntimeError):
    pass


class Redis:
    """One connection per instance; serialize calls if sharing between threads."""
    def __init__(self, host='127.0.0.1', port=6379):
        self.socket = socket.create_connection((host, port), timeout=2)
        self.file = self.socket.makefile('rb')
        self.lock = threading.Lock()

    def _read(self):
        kind = self.file.read(1)
        line = self.file.readline()
        if not kind or not line.endswith(b'\r\n'):
            raise ConnectionError('server closed connection')
        data = line[:-2]
        if kind == b'-':
            raise RedisError(data.decode(errors='replace'))
        if kind == b'+':
            return data
        if kind == b':':
            return int(data)
        if kind == b'$':
            count = int(data)
            if count == -1:
                return None
            value = self.file.read(count)
            if len(value) != count or self.file.read(2) != b'\r\n':
                raise ConnectionError('incomplete response')
            return value
        if kind == b'*':
            return [self._read() for _ in range(int(data))]
        raise RedisError('invalid response')

    def call(self, *args):
        args = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
        frame = b'*%d\r\n' % len(args)
        frame += b''.join(b'$%d\r\n' % len(arg) + arg + b'\r\n' for arg in args)
        with self.lock:
            self.socket.sendall(frame)
            return self._read()

    def close(self):
        self.file.close()
        self.socket.close()


class MockDB:
    def __init__(self, initial=None, failures=0, always_fail=False):
        self.rows = dict(initial or {})
        self.failures = failures
        self.always_fail = always_fail
        self.reads = 0
        self.writes = 0
        self.lock = threading.Lock()

    def get(self, key):
        with self.lock:
            self.reads += 1
            return self.rows.get(key)

    def put(self, key, value):
        with self.lock:
            self.writes += 1
            if self.always_fail or self.failures > 0:
                self.failures = max(0, self.failures - 1)
                raise OSError('mock database unavailable')
            self.rows[key] = value


def cache_aside(redis, db, key, ttl=30):
    value = redis.call('GET', key)
    if value is not None:
        return json.loads(value)
    value = db.get(key)
    if value is not None:
        redis.call('SET', key, json.dumps(value), 'EX', ttl)
    return value


class WriteBack:
    """Process-memory queue, with generation checks to preserve concurrent updates.

    Database writes must have their own bounded timeout in a real integration.
    A successful database write alone permits dequeuing. An interrupted process
    loses its pending queue, even if the Redis cache still has those values.
    """
    def __init__(self, redis, db, retry_delay=0.05):
        self.redis, self.db = redis, db
        self.retry_delay = retry_delay
        self.pending = OrderedDict()
        self.sequence = 0
        self.failures = 0
        self.accepting = True
        self.deadline = None
        self.cv = threading.Condition()
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    def put(self, key, value, ttl=30):
        with self.cv:
            if not self.accepting:
                raise RuntimeError('write-back is shutting down')
            self.redis.call('SET', key, json.dumps(value), 'EX', ttl)
            self.sequence += 1
            self.pending[key] = (self.sequence, value)
            self.cv.notify_all()

    def _run(self):
        while True:
            with self.cv:
                while not self.pending and self.accepting:
                    self.cv.wait()
                if not self.pending:
                    return
                if self.deadline is not None and time.monotonic() >= self.deadline:
                    return
                key, (generation, value) = next(iter(self.pending.items()))
            try:
                self.db.put(key, value)
            except Exception:
                with self.cv:
                    self.failures += 1
                    # Rotate failed keys so one unavailable key does not starve others.
                    if key in self.pending:
                        self.pending.move_to_end(key)
                    remaining = self.retry_delay
                    if self.deadline is not None:
                        remaining = min(remaining, max(0, self.deadline-time.monotonic()))
                    self.cv.wait(remaining)
            else:
                with self.cv:
                    current = self.pending.get(key)
                    if current is not None and current[0] == generation:
                        del self.pending[key]
                    self.cv.notify_all()

    def shutdown(self, timeout=2.0):
        with self.cv:
            self.accepting = False
            self.deadline = time.monotonic() + max(0, timeout)
            self.cv.notify_all()
        self.worker.join(max(0, timeout))
        with self.cv:
            count = len(self.pending)
            return {'pending_failed': count, 'failed_attempts': self.failures,
                    'exit_code': 1 if count else 0}


UNLOCK = """if redis.call('GET',KEYS[1]) == ARGV[1] then
return redis.call('DEL',KEYS[1]) else return 0 end"""


def acquire_lock(redis, key, ttl=5):
    token = secrets.token_hex(24)
    return token if redis.call('SET', key, token, 'NX', 'EX', ttl) == b'OK' else None


def release_lock(redis, key, token):
    return redis.call('EVAL', UNLOCK, 1, key, token) == 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--fail-db', action='store_true')
    args = parser.parse_args()
    redis = Redis(port=args.port)
    try:
        db = MockDB({'example:user:1': {'name': 'Kim'}}, always_fail=args.fail_db)
        print('cache-aside:', cache_aside(redis, db, 'example:user:1'))
        queue = WriteBack(redis, db)
        queue.put('example:user:2', {'name': 'Lee'})
        status = queue.shutdown(timeout=0.5)
        print('write-back shutdown:', json.dumps(status))
        token = acquire_lock(redis, 'example:lock')
        print('token lock:', bool(token), 'release:', release_lock(redis, 'example:lock', token) if token else False)
        return status['exit_code']
    finally:
        redis.close()


if __name__ == '__main__':
    sys.exit(main())
