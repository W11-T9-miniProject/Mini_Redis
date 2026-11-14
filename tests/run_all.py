"""빌드된 실행 파일로 단위 테스트와 전용 TCP/HTTP 서버 기능 테스트를 실행한다.

프로세스와 포트는 테스트가 생성하고 종료한다. 기존 서버를 종료하지 않는다.
"""

import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    suffix = ".exe" if os.name == "nt" else ""
    parser.add_argument("--server", type=Path, default=ROOT / f"build/mini-redis{suffix}")
    parser.add_argument("--unit", type=Path, default=ROOT / f"build/unit-tests{suffix}")
    args = parser.parse_args()
    if not args.server.is_file() or not args.unit.is_file():
        parser.error("실행 파일이 없습니다. 먼저 make 또는 scripts/build-windows.ps1을 실행하세요.")
    subprocess.run([str(args.unit.resolve()), "all"], cwd=ROOT, check=True)
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"],
                   cwd=ROOT, check=True)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([str(args.server.resolve()), str(port)], cwd=ROOT)
    try:
        deadline = time.monotonic() + 8
        while True:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Mini Redis did not start")
                time.sleep(0.05)
        env = dict(os.environ, MR_HOST="127.0.0.1", MR_PORT=str(port))
        subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests/integration", "-v"],
                       cwd=ROOT, env=env, check=True, timeout=90)
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
