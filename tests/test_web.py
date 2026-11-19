import concurrent.futures
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
from urllib.parse import urlsplit

from web.app import SeatService, create_app


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mini-redis-web-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.log = str(Path(cls.temp.name) / "redis.aof")
        cls.process = None
        cls.addClassCleanup(cls.stop_redis)
        cls.start_redis()

    @classmethod
    def start_redis(cls):
        cls.process = subprocess.Popen(["build/mini-redis-server", "0", cls.log],
                                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        assert select.select([cls.process.stdout], [], [], 10)[0]
        line = cls.process.stdout.readline().strip()
        assert line.startswith("LISTENING 127.0.0.1:")
        cls.redis_url = "http://" + line.split()[1]

    @classmethod
    def stop_redis(cls):
        if cls.process and cls.process.poll() is None:
            cls.process.terminate()
            cls.process.wait(timeout=15)
        if cls.process:
            cls.process.stdout.close()

    def setUp(self):
        self.secret = "test-secret-with-enough-entropy-only-for-tests"
        self.admin_key = "test-admin-key-for-local-tests"
        self.service = SeatService(self.redis_url, self.secret, prefix="test:web:" + uuid.uuid4().hex)
        self.service.prepare()
        self.app = create_app(self.service, self.secret, self.admin_key)
        self.app.config["TESTING"] = True

    def participant(self, number, name="김수"):
        client = self.app.test_client()
        response = client.post("/api/join", json={"student_no": number, "full_name": name})
        self.assertEqual(response.status_code, 200)
        return client

    def admin(self):
        client = self.app.test_client()
        self.assertEqual(client.post("/api/admin/login", json={"key": self.admin_key}).status_code, 200)
        return client

    def test_01_full_layout_and_two_available(self):
        response = self.app.test_client().get("/api/state")
        self.assertEqual(response.status_code, 200)
        seats = response.get_json()["seats"]
        self.assertEqual(len(seats), 36)
        self.assertEqual(sum(s["status"] == "available" for s in seats), 2)
        self.assertEqual(sum(s["fixed"] for s in seats), 32)
        self.assertEqual(sum(s["status"] == "vacant" for s in seats), 2)
        active=[s for s in seats if s["status"] != "vacant"]
        for team in range(1,13):
            members=[s for s in active if s["team"] == team]
            count=3 if team<=10 else 2
            self.assertEqual({s["label"] for s in members},{f"{team}팀-{i}" for i in range(1,count+1)})
        self.assertEqual([s["id"] for s in seats if s["status"] == "vacant"],["R-1-1","R-1-2"])
        self.assertNotIn('"owner"', json.dumps(seats))
        self.assertEqual({s["team"] for s in seats if s["status"] == "available"}, {2, 11})
        by_id={s["id"]:s for s in seats}
        expected={"L-4-1":"2팀-1","left-open":"2팀-2","L-4-2":"2팀-3",
                  "R-2-1":"10팀-1","R-3-1":"10팀-2","R-4-1":"10팀-3",
                  "right-open":"11팀-1","R-4-2":"11팀-2",
                  "R-3-2":"12팀-1","R-2-2":"12팀-2"}
        for seat_id,label in expected.items():
            self.assertEqual(by_id[seat_id]["label"],label)
        for zone,team in (("L",3),("M1",5),("M2",7),("M3",9)):
            self.assertEqual(by_id[f"{zone}-3-2"]["label"],f"{team}팀-1")
            self.assertEqual(by_id[f"{zone}-1-2"]["label"],f"{team}팀-3")

    def test_02_names_and_numbers(self):
        self.participant(1, "김")
        self.participant(79, "김수")
        self.participant(90, "남궁민수")
        self.participant(999, "테스트")
        client = self.app.test_client()
        for payload in ({"student_no": 0, "full_name": "김수"},
                        {"student_no": True, "full_name": "김수"},
                        {"student_no": 31, "full_name": ""},
                        {"student_no": 31, "full_name": "김\n수"}):
            self.assertEqual(client.post("/api/join", json=payload).status_code, 400)
        self.assertEqual(client.post("/api/claim", json={"seat_id": "left-open"}).status_code, 401)

    def test_03_claim_confirm_cancel(self):
        client = self.participant(31)
        result = client.post("/api/claim", json={"seat_id": "left-open"})
        self.assertEqual(result.status_code, 200)
        reservation = result.get_json()["reservation"]
        self.assertAlmostEqual(reservation["expires_at_ms"] - time.time() * 1000, 20000, delta=1000)
        body = {"seat_id": "left-open", "reservation_id": reservation["reservation_id"]}
        self.assertEqual(client.post("/api/confirm", json=body).status_code, 200)
        state = client.get("/api/state").get_json()
        mine = next(s for s in state["seats"] if s["is_mine"])
        self.assertEqual(mine["status"], "confirmed")
        self.assertEqual(mine["expires_at_ms"], state["settings"]["reset_at_ms"])
        self.assertEqual(client.post("/api/cancel", json={**body, "expected_state": "held"}).status_code, 409)
        self.assertEqual(client.post("/api/cancel", json={**body, "expected_state": "confirmed"}).status_code, 200)

    def test_04_stale_id_cannot_change_new_reservation(self):
        first = self.participant(31)
        second = self.participant(32, "이수")
        reservation = first.post("/api/claim", json={"seat_id": "left-open"}).get_json()["reservation"]
        key = self.service.seat_key("left-open")
        raw = self.service.store.get_raw(key)
        self.service.store.call({"op": "set", "key": key, "value": raw, "expires_at_ms": 1})
        self.assertEqual(second.post("/api/claim", json={"seat_id": "left-open"}).status_code, 200)
        body = {"seat_id": "left-open", "reservation_id": reservation["reservation_id"]}
        self.assertEqual(first.post("/api/confirm", json=body).status_code, 409)
        self.assertEqual(first.post("/api/cancel", json={**body, "expected_state": "held"}).status_code, 409)
        self.assertTrue(any(s["is_mine"] for s in second.get("/api/state").get_json()["seats"]))

    def test_05_competing_participants(self):
        clients = [self.participant(i + 30, f"교육생{i}") for i in range(12)]
        start = threading.Barrier(len(clients))
        def claim(client):
            start.wait(timeout=15)
            return client.post("/api/claim", json={"seat_id": "left-open"}).status_code
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(clients)) as pool:
            results = list(pool.map(claim, clients))
        self.assertEqual(results.count(200), 1)
        self.assertEqual(results.count(409), len(clients) - 1)

    def test_06_same_student_one_reservation(self):
        first = self.participant(31)
        duplicate = self.app.test_client()
        self.assertEqual(duplicate.post("/api/join", json={"student_no": 31, "full_name": "홍길동"}).status_code, 409)
        self.assertEqual(duplicate.post("/api/join", json={"student_no": 31, "full_name": "김수"}).status_code, 409)
        # Two requests from the SAME authorized browser, not separate identities.
        with first.session_transaction() as identity:
            credentials = dict(identity)
        clients = [self.app.test_client(), self.app.test_client()]
        for client in clients:
            with client.session_transaction() as identity:
                identity.update(credentials)
        start = threading.Barrier(2)
        def claim(args):
            client, seat = args
            start.wait(timeout=15)
            return client.post("/api/claim", json={"seat_id": seat}).status_code
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(claim, zip(clients, ["left-open", "right-open"])))
        self.assertEqual(sorted(results), [200, 409])

    def test_10_shared_visibility_but_private_controls(self):
        first = self.participant(71, "정승영")
        second = self.participant(72, "정승영")  # Same names, distinct numbers.
        reservation = first.post("/api/claim", json={"seat_id": "left-open"}).get_json()["reservation"]
        seats = second.get("/api/state").get_json()["seats"]
        public = next(s for s in seats if s["id"] == "left-open")
        self.assertEqual(public["full_name"], "정승영")
        self.assertFalse(public["is_mine"])
        self.assertNotIn("reservation_id", public)
        payload = {"seat_id": "left-open", "reservation_id": reservation["reservation_id"]}
        self.assertEqual(second.post("/api/cancel", json={**payload, "expected_state": "held"}).status_code, 409)
        self.assertEqual(second.post("/api/confirm", json=payload).status_code, 409)
        self.assertEqual(first.post("/api/confirm", json=payload).status_code, 200)

    def test_11_registration_leave_and_admin_release(self):
        first = self.participant(71, "정승영")
        second = self.app.test_client()
        self.assertEqual(first.post("/api/join", json={"student_no": 71, "full_name": "홍길동"}).status_code, 409)
        self.assertEqual(first.post("/api/join", json={"student_no": 71, "full_name": "정승영"}).status_code, 200)
        reservation = first.post("/api/claim", json={"seat_id": "left-open"}).get_json()["reservation"]
        self.assertEqual(first.post("/api/leave", json={}).status_code, 409)
        admin = self.admin()
        self.assertEqual(admin.post("/api/admin/release", json={"student_no": 71}).status_code, 409)
        self.assertEqual(first.post("/api/cancel", json={"seat_id": "left-open",
                         "reservation_id": reservation["reservation_id"], "expected_state": "held"}).status_code, 200)
        self.assertEqual(admin.post("/api/admin/release", json={"student_no": 71}).status_code, 200)
        self.assertIsNone(first.get("/api/state").get_json()["participant"])
        self.assertEqual(second.post("/api/join", json={"student_no": 71, "full_name": "정승영"}).status_code, 200)
        self.assertEqual(second.post("/api/leave", json={}).status_code, 200)

    def test_18_admin_registration_choices(self):
        first = self.participant(71, "정승영")
        self.participant(23, "민수")
        public = self.app.test_client().get("/api/state").get_json()
        self.assertNotIn("registrations", public)
        admin = self.admin()
        registrations = admin.get("/api/state").get_json()["registrations"]
        self.assertEqual([r["student_no"] for r in registrations], [23, 71])
        self.assertEqual(registrations[1], {"student_no": 71, "full_name": "정승영", "seat_label": None,
                                          "seat_status": None})
        first.post("/api/claim", json={"seat_id": "left-open"})
        held = admin.get("/api/state").get_json()["registrations"][1]
        self.assertEqual((held["seat_label"], held["seat_status"]), ("2팀-2", "held"))
        self.assertEqual(admin.post("/api/admin/release", json={"student_no": 71}).status_code, 409)
        admin.post("/api/admin/cancel", json={"seat_id": "left-open"})
        self.assertEqual(admin.post("/api/admin/release", json={"student_no": 71}).status_code, 200)
        self.assertEqual([r["student_no"] for r in admin.get("/api/state").get_json()["registrations"]], [23])
        # Existing installations have registrations but no discovery catalog.
        self.service.store.call({"op": "delete", "key": self.service.prefix + ":registration-index"})
        self.service.prepare()
        self.assertEqual([r["student_no"] for r in admin.get("/api/state").get_json()["registrations"]], [23])
        self.assertIn('<select id="release-number"', admin.get("/admin").get_data(as_text=True))

    def test_19_qr_participation_page(self):
        client = self.app.test_client()
        empty = client.get("/qr")
        self.assertEqual(empty.status_code, 200)
        self.assertIn("외부 접속 주소를 먼저 설정", empty.get_data(as_text=True))
        self.assertNotIn('class="qr-display-image"', empty.get_data(as_text=True))
        admin = self.admin()
        url = "https://example.trycloudflare.com"
        admin.post("/api/admin/settings", json={"public_url": url})
        page = client.get("/qr")
        self.assertEqual(page.status_code, 200)
        text = page.get_data(as_text=True)
        self.assertIn('class="qr-display-image"', text)
        self.assertIn(url, text)
        self.assertIn('href="/"', text)
        self.assertEqual(client.get("/qr.svg").mimetype, "image/svg+xml")
        for path in ("/", "/admin", "/menu"):
            self.assertIn('href="/qr"', client.get(path).get_data(as_text=True))

    def test_12_legacy_cookies_cannot_own_existing_seat(self):
        legacy = self.app.test_client()
        with legacy.session_transaction() as identity:
            identity.update(student_no=71, full_name="정승영")
        self.assertIsNone(legacy.get("/api/state").get_json()["participant"])
        self.assertEqual(legacy.post("/api/claim", json={"seat_id": "left-open"}).status_code, 401)

    def test_13_presentation_and_assets(self):
        client = self.app.test_client()
        self.assertEqual(client.get("/present").status_code, 302)
        self.assertEqual(urlsplit(client.get("/present").headers["Location"]).path, "/")
        self.assertNotIn(b'href="/present"',client.get("/").data)
        for path in ("/", "/admin", "/static/app.js", "/static/style.css"):
            with client.get(path) as response:
                self.assertEqual(response.status_code, 200)

    def test_07_admin_cancel_and_seed_does_not_reappear(self):
        normal = self.participant(31)
        state = normal.get("/api/state").get_json()
        fixed = next(s["id"] for s in state["seats"] if s["fixed"])
        self.assertEqual(normal.post("/api/admin/cancel", json={"seat_id": fixed}).status_code, 403)
        admin = self.admin()
        self.assertEqual(admin.post("/api/admin/cancel", json={"seat_id": fixed}).status_code, 200)
        self.service.prepare()
        self.assertEqual(sum(s["status"] == "available" for s in self.service.state()["seats"]), 3)

    def test_08_settings_qr_and_input_protection(self):
        client = self.app.test_client()
        self.assertEqual(client.post("/api/admin/login", json={"key": "잘못된키"}).status_code, 401)
        secure = client.post("/api/admin/login", json={"key": self.admin_key},
                             headers={"X-Forwarded-Proto": "https"})
        self.assertIn("Secure;", secure.headers["Set-Cookie"])
        nested = b'{"full_name":' + b'[' * 1100 + b'0' + b']' * 1100 + b',"student_no":31}'
        self.assertEqual(client.post("/api/join", data=nested, content_type="application/json").status_code, 400)
        admin = self.admin()
        future = int(time.time() * 1000) + 60000
        response = admin.post("/api/admin/settings", json={"reset_at_ms": future,
                                                           "public_url": "https://demo.example"})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(admin.get("/qr.svg").status_code, 200)
        self.assertEqual(admin.post("/api/admin/settings", json={"public_url": "javascript:bad"}).status_code, 400)
        for url in ("http://example.com:abc", "http://example.com:99999", "http://example.com:0"):
            self.assertEqual(admin.post("/api/admin/settings", json={"public_url": url}).status_code, 400)
        self.assertEqual(admin.post("/api/admin/settings", json={"public_url": ""}).status_code, 200)
        self.assertEqual(admin.get("/qr.svg").status_code, 400)
        self.assertEqual(admin.post("/api/admin/cancel", json={"seat_id": "left-open"},
                                    headers={"Origin": "https://untrusted.example"}).status_code, 403)
        self.assertEqual(admin.post("/api/join", data=b"x" * 9000,
                                    content_type="application/json").status_code, 413)

    def test_09_redis_restart_preserves_identity_and_fixed_seats(self):
        client = self.participant(31)
        reservation = client.post("/api/claim", json={"seat_id": "right-open"}).get_json()["reservation"]
        self.assertEqual(client.post("/api/confirm", json={"seat_id": "right-open",
                         "reservation_id": reservation["reservation_id"]}).status_code, 200)
        cls = type(self)
        cls.process.kill()
        cls.process.wait(timeout=10)
        cls.process.stdout.close()
        cls.start_redis()
        self.service.store.set_url(cls.redis_url)
        state = client.get("/api/state").get_json()
        self.assertTrue(any(s["is_mine"] and s["status"] == "confirmed" for s in state["seats"]))
        self.assertEqual(sum(s["fixed"] for s in state["seats"]), 32)

    def test_14_vacant_spaces_cannot_be_claimed_or_cancelled(self):
        client=self.participant(71,"정승영")
        admin=self.admin()
        for seat_id in ("R-1-1","R-1-2"):
            self.assertEqual(client.post("/api/claim",json={"seat_id":seat_id}).status_code,400)
            self.assertEqual(admin.post("/api/admin/cancel",json={"seat_id":seat_id}).status_code,400)
        before=client.get("/api/state").get_json()
        fixed=next(s for s in before["seats"] if s["fixed"])
        admin.post("/api/admin/cancel",json={"seat_id":fixed["id"]})
        after=client.get("/api/state").get_json()
        freed=next(s for s in after["seats"] if s["id"] == fixed["id"])
        self.assertEqual(freed["label"],fixed["label"])
        self.assertEqual(freed["status"],"available")

    def test_15_legacy_reservation_is_not_silently_hidden(self):
        key=self.service.prefix+":seat:R-1-1"
        self.service.store.put(key,{"owner":"legacy-user","fixed":False,"student_no":71})
        before=self.service.store.get_raw(key)
        from web.app import AppError
        with self.assertRaises(AppError):
            self.service.prepare()
        self.assertEqual(self.service.store.get_raw(key),before)

    def test_16_admin_restore_original_assignment(self):
        admin=self.admin()
        seat_id="M1-1-1"
        admin.post("/api/admin/cancel",json={"seat_id":seat_id})
        self.assertEqual(self.app.test_client().post("/api/admin/restore",json={"seat_id":seat_id}).status_code,403)
        self.assertEqual(admin.post("/api/admin/restore",json={"seat_id":seat_id}).status_code,200)
        restored=next(s for s in admin.get("/api/state").get_json()["seats"] if s["id"]==seat_id)
        self.assertTrue(restored["fixed"])
        self.assertEqual(restored["full_name"],"기존 배정")
        self.assertEqual(restored["expires_at_ms"],0)
        self.assertEqual(admin.post("/api/admin/restore",json={"seat_id":seat_id}).status_code,409)
        for invalid in ("left-open","right-open","R-1-1","bad"):
            self.assertEqual(admin.post("/api/admin/restore",json={"seat_id":invalid}).status_code,400)

    def test_17_restore_races_with_claim_without_overwriting(self):
        admin=self.admin()
        participant=self.participant(71,"정승영")
        seat_id="M1-1-1"
        admin.post("/api/admin/cancel",json={"seat_id":seat_id})
        start=threading.Barrier(2)
        def request(args):
            client,path=args
            start.wait(timeout=10)
            return client.post(path,json={"seat_id":seat_id}).status_code
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results=list(pool.map(request,[(admin,"/api/admin/restore"),(participant,"/api/claim")]))
        self.assertEqual(sorted(results),[200,409])
        saved=self.service.store.get(self.service.seat_key(seat_id))
        self.assertEqual(saved["owner"]=="preset",results[0]==200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
