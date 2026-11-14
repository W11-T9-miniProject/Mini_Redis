"""Exact 256 wire-byte writes, 50 connections, pipeline 16, 1M responses.

Memcached uses its native text protocol with identical value payload and total
request bytes (extra spaces in its command header). No performance threshold is
invented. Run each server separately on localhost; persist stdout as JSON.
"""
import argparse
import concurrent.futures
import json
import platform
import socket
import threading
import time


def request(protocol, key):
    value = b'x' * 191
    if protocol == 'redis':
        args = [b'SET', key, value, b'EX', b'300']
        wire = b'*5\r\n' + b''.join(b'$%d\r\n' % len(a) + a + b'\r\n' for a in args)
        # Redis bulk strings cannot be padded; key length fixes wire size.
    else:
        header = b'set ' + key + b' 0 300 191'
        wire = header + b' ' * (256 - len(header) - len(value) - 4) + b'\r\n' + value + b'\r\n'
    assert len(wire) == 256, len(wire)
    return wire


def run(protocol, port):
    barrier = threading.Barrier(51)
    def worker(index):
        # 20-byte keys make this RESP request exactly 256 bytes.
        key = ('bench:%014d' % index).encode()
        wire = request(protocol, key)
        good = bad = 0
        with socket.create_connection(('127.0.0.1', port), timeout=60) as conn:
            reader = conn.makefile('rb')
            barrier.wait(timeout=60)
            for start in range(0, 20000, 16):
                count = min(16, 20000 - start)
                conn.sendall(wire * count)
                for _ in range(count):
                    line = reader.readline()
                    if not line:
                        raise RuntimeError('server closed connection')
                    if line == (b'+OK\r\n' if protocol == 'redis' else b'STORED\r\n'):
                        good += 1
                    else:
                        bad += 1
        return good, bad
    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as pool:
        futures = [pool.submit(worker, i) for i in range(50)]
        start = time.perf_counter()
        barrier.wait(timeout=60)
        results = [f.result() for f in futures]
        elapsed = time.perf_counter() - start
    good = sum(x[0] for x in results)
    bad = sum(x[1] for x in results)
    print(json.dumps(dict(protocol=protocol, host='127.0.0.1', port=port,
                         connections=50, pipeline=16, request_bytes=256,
                         value_bytes=191, requests=1000000, success=good,
                         errors=bad, elapsed_seconds=elapsed,
                         requests_per_second=1000000 / elapsed,
                         platform=platform.platform(), python=platform.python_version(),
                         threshold='unspecified'), indent=2))
    return 0 if good == 1000000 and bad == 0 else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', choices=['redis', 'memcached'], default='redis')
    parser.add_argument('--port', type=int, default=6380)
    args = parser.parse_args()
    raise SystemExit(run(args.protocol, args.port))
