import argparse
import hashlib
import hmac
import http.client
import io
import json
import os
from pathlib import Path
import secrets
import threading
import time
import unicodedata
from urllib.parse import urlsplit

from flask import Flask, jsonify, redirect, render_template, request, send_file, session
from flask.sessions import SecureCookieSessionInterface
import qrcode
import qrcode.image.svg
from werkzeug.exceptions import HTTPException

from .layout import OPEN_IDS, SEATS, SEAT_IDS
from .errors import AppError


def now_ms():
    return int(time.time() * 1000)


def preset_assignment():
    return {"owner": "preset", "full_name": "기존 배정", "student_no": None,
            "reservation_id": "preset", "state": "confirmed", "fixed": True, "expires_at_ms": 0}


class DemoSessionInterface(SecureCookieSessionInterface):
    def get_cookie_secure(self, app):
        # HTTPS tunnels set this header; local HTTP remains available for the demo.
        return (super().get_cookie_secure(app) or request.is_secure or
                request.headers.get("X-Forwarded-Proto", "").lower() == "https")


class MiniRedis:
    def __init__(self, url):
        self.set_url(url)

    def set_url(self, url):
        parsed = urlsplit(url)
        if parsed.scheme != "http" or not parsed.hostname:
            raise ValueError("Mini Redis URL must be an http:// address")
        self.host, self.port = parsed.hostname, parsed.port or 80

    def call(self, payload):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=3)
        try:
            connection.request("POST", "/kv", json.dumps(payload, ensure_ascii=False).encode(),
                               {"Content-Type": "application/json"})
            response = connection.getresponse()
            body = json.loads(response.read(2 * 1024 * 1024))
            if response.status not in (200, 404, 409):
                raise AppError("저장소가 요청을 완료하지 못했습니다. 현재 상태를 다시 확인해 주세요.", 503)
            return response.status, body
        except (OSError, http.client.HTTPException, ValueError) as exc:
            raise AppError("저장소 연결을 확인할 수 없습니다. 요청 결과를 다시 확인해 주세요.", 503) from exc
        finally:
            connection.close()

    def get_raw(self, key):
        code, body = self.call({"op": "get", "key": key})
        return None if code == 404 else body["value"]

    def get(self, key):
        raw = self.get_raw(key)
        if raw is None:
            return None
        try:
            value = json.loads(raw)
            if not isinstance(value, dict):
                raise ValueError("expected object")
            return value
        except ValueError as exc:
            raise AppError("저장된 시연 데이터 형식을 확인해 주세요.", 503) from exc

    def put(self, key, value, **options):
        return self.call({"op": "set", "key": key, "value": self.encode(value), **options})

    @staticmethod
    def encode(value):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class SeatService:
    def __init__(self, redis_url, secret, prefix="jungle:seats:v1"):
        self.store = MiniRedis(redis_url)
        self.secret = secret.encode()
        self.prefix = prefix
        self.locks = {}
        self.locks_guard = threading.Lock()
        self.registration_index_lock = threading.Lock()

    def seat_key(self, seat):
        if not isinstance(seat, str) or seat not in SEAT_IDS:
            raise AppError("존재하지 않는 자리입니다.")
        return self.prefix + ":seat:" + seat

    def owner(self, token):
        return hmac.new(self.secret, token.encode(), hashlib.sha256).hexdigest()

    def registration_key(self, number):
        return self.prefix + ":participant:" + str(number)

    def authenticated(self, number, name, token):
        if not isinstance(token, str) or len(token) != 64:
            return False
        registration = self.store.get(self.registration_key(number))
        return bool(registration and registration.get("full_name") == name and
                    secrets.compare_digest(registration.get("owner", ""), self.owner(token)))

    def register(self, number, name, token):
        with self.user_lock(number):
            key = self.registration_key(number)
            existing = self.store.get(key)
            if existing:
                if existing.get("full_name") != name:
                    raise AppError("이미 등록된 교육생 번호입니다. 다른 이름으로 사용할 수 없습니다.", 409)
                if not secrets.compare_digest(existing.get("owner", ""), self.owner(token)):
                    raise AppError("다른 브라우저에서 등록한 번호입니다. 기기를 바꾸려면 관리자에게 등록 초기화를 요청해 주세요.", 409)
                return
            if self.find_mine(number):
                raise AppError("기존 테스트 배정이 남아 있습니다. 관리자가 해당 배정을 취소한 뒤 다시 참여해 주세요.", 409)
            # Index first: failed joins may leave an unused number, never an undiscoverable registration.
            with self.registration_index_lock:
                index_key = self.prefix + ":registration-index"
                index = self.store.get(index_key) or {"numbers": []}
                if number not in index["numbers"]:
                    index["numbers"].append(number)
                    self.store.put(index_key, index)
            code, _ = self.store.put(key, {"full_name": name, "owner": self.owner(token)}, nx=True)
            if code != 200:
                raise AppError("이미 등록된 교육생 번호입니다.", 409)

    def release_registration(self, number, token=None):
        with self.user_lock(number):
            if self.find_mine(number):
                raise AppError("현재 배정 또는 선점을 먼저 취소해 주세요.", 409)
            key = self.registration_key(number)
            raw = self.store.get_raw(key)
            if raw is None:
                return
            record = json.loads(raw)
            if token and not secrets.compare_digest(record.get("owner", ""), self.owner(token)):
                raise AppError("현재 브라우저의 등록 정보가 아닙니다.", 409)
            code, _ = self.store.call({"op": "compare_delete", "key": key, "expected": raw})
            if code not in (200, 404):
                raise AppError("등록 정보가 변경되었습니다. 다시 확인해 주세요.", 409)

    def user_lock(self, number):
        with self.locks_guard:
            return self.locks.setdefault(number, threading.Lock())

    def settings(self):
        value = self.store.get(self.prefix + ":settings")
        if value is None:
            raise AppError("시연 설정을 찾을 수 없습니다.", 503)
        return value

    def registrations(self, seats):
        index = self.store.get(self.prefix + ":registration-index") or {"numbers": []}
        occupied = {s["student_no"]: s for s in seats if s.get("student_no") is not None}
        result = []
        for number in sorted(index["numbers"]):
            record = self.store.get(self.registration_key(number))
            if record is None:
                continue
            seat = occupied.get(number)
            result.append({"student_no": number, "full_name": record["full_name"],
                           "seat_label": seat["label"] if seat else None,
                           "seat_status": seat["status"] if seat else None})
        return result

    def prepare(self):
        # Do not silently hide a user's reservation when loading an older layout.
        for seat in SEATS:
            if not seat["selectable"]:
                legacy = self.store.get(self.prefix + ":seat:" + seat["id"])
                if legacy and not (legacy.get("fixed") and legacy.get("owner") == "preset"):
                    raise AppError("공석으로 바꿀 자리에 기존 예약이 남아 있습니다. 이전 배치에서 취소한 뒤 다시 실행해 주세요.", 409)
        prepared = self.store.get(self.prefix + ":prepared")
        if self.store.get(self.prefix + ":registration-index") is None:
            # One-time upgrade for registrations created before the discovery index existed.
            numbers = [n for n in range(1, 10000) if self.store.get(self.registration_key(n))] if prepared else []
            self.store.put(self.prefix + ":registration-index", {"numbers": numbers}, nx=True)
        if prepared:
            return
        self.store.put(self.prefix + ":settings", {"reset_at_ms": now_ms() + 86400000,
                                                   "public_url": ""}, nx=True)
        for seat in SEATS:
            if seat["selectable"] and seat["id"] not in OPEN_IDS:
                self.store.put(self.seat_key(seat["id"]), preset_assignment(), nx=True)
        # Marker is last: a failed initial seed can resume without overwriting data.
        self.store.put(self.prefix + ":prepared", {"prepared": True})

    def state(self, student_no=None, token=None):
        owner = self.owner(token) if token else None
        result = []
        for seat in SEATS:
            if not seat["selectable"]:
                result.append({**seat, "status": "vacant", "fixed": False, "is_mine": False,
                               "full_name": "", "student_no": None, "expires_at_ms": 0})
                continue
            record = self.store.get(self.seat_key(seat["id"]))
            mine = bool(record and record.get("owner") == owner)
            item = {**seat, "status": record["state"] if record else "available",
                    "fixed": bool(record and record.get("fixed")), "is_mine": mine,
                    "full_name": record["full_name"] if record else "",
                    "student_no": record.get("student_no") if record else None,
                    "expires_at_ms": record.get("expires_at_ms", 0) if record else 0}
            if mine:
                item["reservation_id"] = record["reservation_id"]
            result.append(item)
        return {"seats": result, "settings": self.settings(), "server_now_ms": now_ms()}

    def find_mine(self, number):
        for seat in SEATS:
            if not seat["selectable"]:
                continue
            record = self.store.get(self.seat_key(seat["id"]))
            if record and record.get("student_no") == number:
                return record
        return None

    def restore(self, seat):
        key = self.seat_key(seat)
        if seat in OPEN_IDS:
            raise AppError("시연용 두 빈자리는 기존 배정 복구 대상이 아닙니다.")
        code, _ = self.store.put(key, preset_assignment(), nx=True)
        if code != 200:
            raise AppError("이미 선점 또는 배정된 자리입니다. 현재 상태를 확인해 주세요.", 409)

    def claim(self, number, name, seat, token):
        key = self.seat_key(seat)
        with self.user_lock(number):
            if not self.authenticated(number, name, token):
                raise AppError("등록 정보가 만료되었습니다. 다시 참여해 주세요.", 401)
            if self.find_mine(number):
                raise AppError("이미 선점하거나 확정한 자리가 있습니다.", 409)
            record = {"owner": self.owner(token), "student_no": number, "full_name": name,
                      "reservation_id": secrets.token_hex(16), "state": "held", "fixed": False,
                      "expires_at_ms": now_ms() + 20000}
            code, _ = self.store.put(key, record, nx=True, ttl_ms=20000)
            if code == 409:
                raise AppError("다른 교육생이 먼저 선점한 자리입니다.", 409)
            return {"seat_id": seat, **{k: v for k, v in record.items() if k != "owner"}}

    def change(self, number, name, token, seat, reservation_id, confirm, expected_state="held"):
        key = self.seat_key(seat)
        with self.user_lock(number):
            if not self.authenticated(number, name, token):
                raise AppError("등록 정보가 만료되었습니다. 다시 참여해 주세요.", 401)
            raw = self.store.get_raw(key)
            record = json.loads(raw) if raw is not None else None
            if (not record or record.get("owner") != self.owner(token) or
                    record.get("reservation_id") != reservation_id or
                    record.get("state") != expected_state):
                raise AppError("선점이 만료되었거나 현재 선점 정보와 일치하지 않습니다.", 409)
            if confirm:
                deadline = self.settings()["reset_at_ms"]
                if deadline <= now_ms():
                    raise AppError("배정 만료 시각이 지났습니다. 관리자에게 문의해 주세요.", 409)
                updated = {**record, "state": "confirmed", "expires_at_ms": deadline}
                payload = {"op": "compare_set", "key": key, "expected": raw,
                           "value": self.store.encode(updated), "expires_at_ms": deadline}
            else:
                payload = {"op": "compare_delete", "key": key, "expected": raw}
            code, _ = self.store.call(payload)
            if code != 200:
                raise AppError("처리 중 선점 상태가 바뀌었습니다. 현재 상태를 확인해 주세요.", 409)


def create_app(service, secret, admin_key, menu_service=None):
    from .menu import MenuService, TTL_SECONDS
    menu_service = menu_service or MenuService(service.store, service.prefix + ":cache:menu:114707701")
    app = Flask(__name__)
    app.secret_key = secret
    app.session_interface = DemoSessionInterface()
    app.config.update(MAX_CONTENT_LENGTH=8192, SESSION_COOKIE_HTTPONLY=True,
                      SESSION_COOKIE_SAMESITE="Lax",
                      SESSION_COOKIE_SECURE=os.environ.get("MINI_REDIS_COOKIE_SECURE") == "1")

    def clear_participant():
        for key in ("student_no", "full_name", "browser_token"):
            session.pop(key, None)

    def participant(required=True):
        number, name, token = session.get("student_no"), session.get("full_name"), session.get("browser_token")
        if not number or not service.authenticated(number, name, token):
            clear_participant()
            if required:
                raise AppError("먼저 성명과 교육생 번호로 다시 참여해 주세요.", 401)
            return None
        return number, name, token

    def administrator():
        if not session.get("admin"):
            raise AppError("관리자 인증이 필요합니다.", 403)

    def body(fields, required=None):
        if request.mimetype != "application/json":
            raise AppError("JSON 요청이 필요합니다.", 415)
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result:
                    raise ValueError("duplicate field")
                result[key] = value
            return result
        def invalid_constant(_):
            raise ValueError("non-finite number")
        try:
            if request.content_length and request.content_length > 8192:
                raise AppError("요청 크기가 너무 큽니다.", 413)
            raw = request.stream.read(8193)
            if len(raw) > 8192:
                raise AppError("요청 크기가 너무 큽니다.", 413)
            value = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                               parse_constant=invalid_constant)
        except (ValueError, UnicodeError, RecursionError):
            raise AppError("요청 형식이 올바르지 않습니다.")
        if (not isinstance(value, dict) or set(value) - set(fields) or
                set(required if required is not None else fields) - set(value)):
            raise AppError("요청 항목을 확인해 주세요.")
        return value

    @app.before_request
    def check_origin():
        if request.method == "POST" and request.headers.get("Origin"):
            origin = request.headers["Origin"].rstrip("/")
            allowed = {"http://" + request.host, "https://" + request.host}
            if origin not in allowed:
                raise AppError("다른 사이트에서 보낸 변경 요청은 허용하지 않습니다.", 403)

    @app.after_request
    def headers(response):
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' https://k.kakaocdn.net; frame-ancestors 'none'; base-uri 'none'"
        return response

    @app.errorhandler(AppError)
    def app_error(error):
        return jsonify(error=error.message), error.code

    @app.errorhandler(HTTPException)
    def http_error(error):
        return jsonify(error=error.description), error.code

    @app.get("/")
    def index():
        return render_template("index.html", admin=False)

    @app.get("/admin")
    def admin_page():
        return render_template("index.html", admin=True)

    @app.get("/present")
    def presentation():
        return redirect("/")

    @app.get("/api/state")
    def state():
        identity = participant(required=False)
        result = service.state(identity[0], identity[2]) if identity else service.state()
        result["participant"] = {"student_no": identity[0], "full_name": identity[1]} if identity else None
        result["admin"] = bool(session.get("admin"))
        if result["admin"]:
            result["registrations"] = service.registrations(result["seats"])
        return jsonify(result)

    @app.get("/menu")
    def menu_page():
        return render_template("menu.html", admin=bool(session.get("admin")), cache_minutes=TTL_SECONDS // 60)

    @app.get("/api/menu")
    def menu_get():
        return jsonify(menu_service.get())

    @app.post("/api/admin/menu/origin")
    def menu_origin():
        administrator()
        body(set())
        return jsonify(menu_service.get(use_cache=False))

    @app.post("/api/admin/menu/clear")
    def menu_clear():
        administrator()
        body(set())
        menu_service.clear()
        return jsonify(status="ok")

    @app.post("/api/admin/menu/benchmark")
    def menu_benchmark():
        administrator()
        body(set())
        return jsonify(menu_service.benchmark())

    @app.post("/api/join")
    def join():
        value = body({"student_no", "full_name"})
        number, name = value["student_no"], value["full_name"]
        if not isinstance(name, str):
            raise AppError("성명을 확인해 주세요.")
        name = unicodedata.normalize("NFC", name.strip())
        if type(number) is not int or not 1 <= number <= 9999 or not 1 <= len(name) <= 30 or not name.isprintable():
            raise AppError("성명과 양의 정수 교육생 번호를 확인해 주세요.")
        old = session.get("student_no")
        if old and old != number and session.get("browser_token"):
            raise AppError("참여 정보 변경 버튼으로 기존 등록을 종료한 뒤 입력해 주세요.", 409)
        token = session.get("browser_token") or secrets.token_hex(32)
        service.register(number, name, token)
        session.update(student_no=number, full_name=name, browser_token=token)
        return jsonify(status="ok")

    @app.post("/api/leave")
    def leave():
        body(set())
        identity = participant(required=False)
        if identity:
            service.release_registration(identity[0], identity[2])
        clear_participant()
        return jsonify(status="ok")

    @app.post("/api/claim")
    def claim():
        number, name, token = participant()
        value = body({"seat_id"})
        return jsonify(status="ok", reservation=service.claim(number, name, value["seat_id"], token))

    @app.post("/api/confirm")
    def confirm():
        number, name, token = participant()
        value = body({"seat_id", "reservation_id"})
        service.change(number, name, token, value["seat_id"], value["reservation_id"], True)
        return jsonify(status="ok")

    @app.post("/api/cancel")
    def cancel():
        number, name, token = participant()
        value = body({"seat_id", "reservation_id", "expected_state"})
        if value["expected_state"] not in ("held", "confirmed"):
            raise AppError("취소할 상태를 확인해 주세요.")
        service.change(number, name, token, value["seat_id"], value["reservation_id"], False, value["expected_state"])
        return jsonify(status="ok")

    @app.post("/api/admin/login")
    def admin_login():
        value = body({"key"})
        if (not isinstance(value["key"], str) or not value["key"].isascii() or
                not secrets.compare_digest(value["key"], admin_key)):
            raise AppError("관리자 키가 올바르지 않습니다.", 401)
        session["admin"] = True
        return jsonify(status="ok")

    @app.post("/api/admin/cancel")
    def admin_cancel():
        administrator()
        value = body({"seat_id"})
        service.store.call({"op": "delete", "key": service.seat_key(value["seat_id"])})
        return jsonify(status="ok")

    @app.post("/api/admin/release")
    def admin_release():
        administrator()
        value = body({"student_no"})
        if type(value["student_no"]) is not int or not 1 <= value["student_no"] <= 9999:
            raise AppError("교육생 번호를 확인해 주세요.")
        service.release_registration(value["student_no"])
        return jsonify(status="ok")

    @app.post("/api/admin/restore")
    def admin_restore():
        administrator()
        value = body({"seat_id"})
        service.restore(value["seat_id"])
        return jsonify(status="ok")

    @app.post("/api/admin/settings")
    def admin_settings():
        administrator()
        value = body({"reset_at_ms", "public_url"}, required=set())
        settings = service.settings()
        if "reset_at_ms" in value:
            deadline = value["reset_at_ms"]
            if type(deadline) is not int or not now_ms() < deadline <= now_ms() + 7 * 86400000:
                raise AppError("만료 시각은 현재부터 7일 이내로 지정해 주세요.")
            settings["reset_at_ms"] = deadline
        if "public_url" in value:
            url = value["public_url"]
            if not isinstance(url, str):
                raise AppError("접속 주소를 확인해 주세요.")
            if not url:
                settings["public_url"] = ""
                service.store.put(service.prefix + ":settings", settings)
                return jsonify(status="ok")
            try:
                parsed = urlsplit(url)
                port = parsed.port
            except ValueError:
                raise AppError("접속 주소를 확인해 주세요.")
            if (parsed.scheme not in ("http", "https") or not parsed.hostname or
                    parsed.username or parsed.password or parsed.path not in ("", "/") or
                    parsed.query or parsed.fragment or (port is not None and not 1 <= port <= 65535)):
                raise AppError("http 또는 https 접속 주소를 입력해 주세요.")
            settings["public_url"] = url.rstrip("/")
        service.store.put(service.prefix + ":settings", settings)
        return jsonify(status="ok")

    @app.get("/qr")
    def qr_page():
        return render_template("qr.html", public_url=service.settings().get("public_url"))

    @app.get("/qr.svg")
    def qr():
        url = service.settings().get("public_url")
        if not url:
            raise AppError("관리자 화면에서 외부 접속 주소를 먼저 설정해 주세요.")
        output = io.BytesIO()
        qrcode.make(url, image_factory=qrcode.image.svg.SvgPathImage, border=3).save(output)
        output.seek(0)
        return send_file(output, mimetype="image/svg+xml")

    return app


def file_secret(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return path.read_text().strip()
    value = secrets.token_hex(32)
    with os.fdopen(fd, "w") as output:
        output.write(value + "\n")
        output.flush()
        os.fsync(output.fileno())
    return value


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--redis-url", default="http://127.0.0.1:8080")
    args = parser.parse_args()
    secret = file_secret(Path("data/web-session.key"))
    admin_key = file_secret(Path("data/web-admin.key"))
    service = SeatService(args.redis_url, secret)
    service.prepare()
    app = create_app(service, secret, admin_key)
    print("관리자 키 파일: data/web-admin.key", flush=True)
    app.run(host=args.host, port=args.port, threaded=True, debug=False, use_reloader=False)
