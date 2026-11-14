"""Coordinates follow the supplied classroom diagram (1760 × 980)."""

SEATS = []
for zone, xs, ys in [
    ("L", (16, 94), (175, 357, 539, 721)),
    ("M1", (408, 486), (357, 539, 721)),
    ("M2", (800, 878), (357, 539, 721)),
    ("M3", (1192, 1270), (357, 539, 721)),
    ("R", (1584, 1662), (175, 357, 539, 721)),
]:
    for row, y in enumerate(ys, 1):
        for column, x in enumerate(xs, 1):
            SEATS.append({"id": f"{zone}-{row}-{column}", "label": f"{zone} {row}-{column}",
                          "x": x, "y": y, "w": 74, "h": 175, "team": None})
SEATS += [
    {"id": "left-open", "label": "왼쪽 끝자리 · 2팀", "team": 2,
     "x": 3, "y": 906, "w": 175, "h": 72},
    {"id": "right-open", "label": "오른쪽 끝자리 · 11팀", "team": 11,
     "x": 1570, "y": 906, "w": 175, "h": 72},
]
OPEN_IDS = {"left-open", "right-open"}

# Keep storage IDs stable so existing reservations survive display-name changes.
# Number down the left side, around the bottom, then up the right side.
TEAM_SEATS = {
    1: ["L-1-1", "L-2-1", "L-3-1"],
    2: ["L-4-1", "left-open", "L-4-2"],
    3: ["L-3-2", "L-2-2", "L-1-2"],
    4: ["M1-1-1", "M1-2-1", "M1-3-1"],
    5: ["M1-3-2", "M1-2-2", "M1-1-2"],
    6: ["M2-1-1", "M2-2-1", "M2-3-1"],
    7: ["M2-3-2", "M2-2-2", "M2-1-2"],
    8: ["M3-1-1", "M3-2-1", "M3-3-1"],
    9: ["M3-3-2", "M3-2-2", "M3-1-2"],
    10: ["R-2-1", "R-3-1", "R-4-1"],
    11: ["right-open", "R-4-2"],
    12: ["R-3-2", "R-2-2"],
}
by_id = {seat["id"]: seat for seat in SEATS}
for team, ids in TEAM_SEATS.items():
    for position, seat_id in enumerate(ids, 1):
        by_id[seat_id].update(team=team, position=position, label=f"{team}팀-{position}", selectable=True,
                              demo=seat_id in OPEN_IDS)
for position, seat_id in enumerate(("R-1-1", "R-1-2"), 1):
    by_id[seat_id].update(team=None, position=position, label=f"공석-{position}", selectable=False, demo=False)
SEATS.sort(key=lambda seat: (seat["team"] or 99, seat["position"]))
SEAT_IDS = {seat["id"] for seat in SEATS if seat["selectable"]}
