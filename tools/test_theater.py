"""Saved films (port/linux/game/theater.c, theater_film.c): a system link game of two copies on the loopback
(tools/net_compare.py, scripted players with hits and kills), both recording (game.record_films); then each film played
without a person (debug.film_playback, logged as debug.network_test "log" logs a game), whose every second must be
the game its machine logged: a client's film the client's game, a host's the host's. A film another network version
made is refused, and a damaged one plays up to the damage. Needs the retail maps in assets/maps and ninja linux's
build/linux/halo; the game draws nothing (the null renderer), with no audio, internet play or update check."""

import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RETAIL_MAPS = ROOT / "assets/maps"
GAME = ROOT / "build/linux/halo"
# a second of the network test's log (network_test.c): its tick, and each player's place (or dead), shields and
# health, where they aim (degrees: yaw and pitch), score, kills and deaths
TICK = re.compile(r"network test: tick (\d+) (.*)")
PLAYER = re.compile(r"player (\d+): (?:\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) h([\d.]+)/([\d.]+)[^|]*? a(-?\d+)/(-?\d+) |dead)"
                    r"[^|]*? s(-?\d+) k(\d+) d(\d+)")
# a film's header (theater_film.c's struct theater_film_header): its network version's offset
NETWORK_VERSION_OFFSET = 12
# how far apart a player may be (world units): a client's film is the client's own game, as it ran it, but for what
# its frames did between ticks; a host's is the host's game as a client sees the other machines' players, kept to
# within the netcode's correction of them (REMOTE_CORRECTION_TOLERANCE, network_distributed.h), with a margin for
# the half of the way each tick it is moved
CLIENT_DISTANCE = 0.06
HOST_DISTANCE = 0.1
# shields and health: a host's film has the host's word a tick before the client's own tick runs (a recharge apart)
VITALITY = 0.02
# where a player aims (degrees, as logged: whole ones): a client's film is its own game; a host's turns another
# machine's player by the input the host relayed, a tick behind the host's own at most (a scripted player turns
# about 3 degrees a tick)
CLIENT_AIM = 2
HOST_AIM = 5


def _game():
    if not GAME.is_file() or not (RETAIL_MAPS / "bloodgulch.map").is_file():
        pytest.skip("needs bloodgulch.map in assets/maps and ninja linux's build/linux/halo")


def _seconds(text):
    """each second logged: {tick: {player: (place or None, (shields, health), (yaw, pitch), (score, kills,
    deaths))}}"""
    seconds = {}
    for line in text.splitlines():
        tick = TICK.search(line)
        if not tick:
            continue
        players = {}
        for match in PLAYER.finditer(tick.group(2)):
            place = tuple(float(match.group(index)) for index in (2, 3, 4)) if match.group(2) else None
            vitality = (float(match.group(5)), float(match.group(6))) if match.group(2) else None
            aim = (int(match.group(7)), int(match.group(8))) if match.group(2) else None
            players[int(match.group(1))] = (place, vitality, aim,
                                            tuple(int(match.group(index)) for index in (9, 10, 11)))
        seconds[int(tick.group(1))] = players
    return seconds


@pytest.fixture(scope="module")
def recorded(tmp_path_factory):
    """a game played by two copies, each recording: {"host"/"client": (its log, its film)}"""
    _game()
    work = tmp_path_factory.mktemp("theater")
    settings = []
    for copy in ("a", "b"):
        settings += [f"--{copy}-env", "HALO_RECORD_FILMS=true",
                     # (no high-res HUD: the null renderer draws none of its pictures, and the scores at a game's end
                     # would ask it to)
                     f"--{copy}-env", "HALO_HIGH_RES_HUD=false"]
    result = subprocess.run([sys.executable, str(ROOT / "tools/net_compare.py"), "--a", str(GAME),
                             "--data", str(RETAIL_MAPS.parent), "--maps", "bloodgulch", "--seconds", "30", "--events",
                             "--work", str(work / "game"), "--keep", *settings], cwd=ROOT, capture_output=True,
                            text=True, timeout=600)
    assert "1 of 1 games in step" in result.stdout, result.stdout[-3000:] + result.stderr[-2000:]
    run = next((work / "game/runs").glob("bloodgulch.AB.*"))
    films = {}
    for role, copy in (("host", "A"), ("client", "B")):
        found = sorted((work / "game" / copy / "saves/films").glob("*.film"))
        assert len(found) == 1, f"the {role} wrote {len(found)} films"
        films[role] = ((run / f"{role}.log").read_text(errors="replace"), found[0])
    return work, films


def _play(work, film, name, seconds=180):
    """the film played without a person until it is over: the game's log"""
    folder = work / name
    (folder / "bin").mkdir(parents=True)
    for source in [GAME, *GAME.parent.glob("lib*.so*")]:
        shutil.copy(source, folder / "bin" / source.name)
    (folder / "data").mkdir()
    (folder / "data/maps").symlink_to(RETAIL_MAPS)
    env = {name: value for name, value in os.environ.items()
           if not name.startswith("HALO_") and name not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"),
               HALO_FILM_PLAYBACK=str(film), HALO_NETWORK_TEST="log", HALO_NULL_RENDERER="1",
               SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy", HALO_NO_AUDIO="1", HALO_NET_ONLINE="false",
               HALO_UPDATE_AUTO="false", HALO_NET_ADDRESS="127.10.97.10", HALO_NET_BROADCAST="127.10.97.11",
               HALO_HIGH_RES_HUD="false", HALO_EXIT_AFTER=str(seconds))
    log = folder / "game.log"
    with open(log, "w") as output:
        game = subprocess.Popen([str(folder / "bin" / GAME.name)], cwd=folder / "bin", env=env, stdout=output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + seconds
        try:
            while time.monotonic() < deadline and game.poll() is None:
                text = log.read_text(errors="replace")
                if re.search(r"theater: (the film is over|could not)", text):
                    time.sleep(1.0)
                    break
                time.sleep(0.2)
        finally:
            game.kill()
            game.wait()
    return log.read_text(errors="replace")


@pytest.mark.parametrize("role,distance,aim_tolerance", [("client", CLIENT_DISTANCE, CLIENT_AIM),
                                                         ("host", HOST_DISTANCE, HOST_AIM)])
def test_a_film_plays_its_game_again(recorded, role, distance, aim_tolerance):
    work, films = recorded
    log, film = films[role]
    played = _play(work, film, f"play-{role}")
    assert "theater: the film is over" in played, played[-3000:]
    assert not re.search(r"assert|exception|segmentation fault", played, re.I), played[-3000:]
    expected, seen = _seconds(log), _seconds(played)
    common = sorted(set(expected) & set(seen))
    # (every second of the game but its last few, which the film may end before)
    assert len(common) >= len(expected) - 3, (sorted(expected), sorted(seen))
    for tick in common:
        for player, (place, vitality, aim, scores) in expected[tick].items():
            assert player in seen[tick], (tick, player)
            other_place, other_vitality, other_aim, other_scores = seen[tick][player]
            assert other_scores == scores, (tick, player, scores, other_scores)
            assert (place is None) == (other_place is None), (tick, player, place, other_place)
            if place:
                assert math.dist(place, other_place) <= distance, (tick, player, place, other_place)
                assert max(abs(a - b) for a, b in zip(vitality, other_vitality)) <= VITALITY, \
                    (tick, player, vitality, other_vitality)
                # (yaw round the circle)
                assert min(abs(aim[0] - other_aim[0]), 360 - abs(aim[0] - other_aim[0])) <= aim_tolerance and \
                    abs(aim[1] - other_aim[1]) <= aim_tolerance, (tick, player, aim, other_aim)


def test_a_film_of_another_network_version_is_refused(recorded):
    work, films = recorded
    data = bytearray(films["client"][1].read_bytes())
    version = struct.unpack_from("<I", data, NETWORK_VERSION_OFFSET)[0]
    struct.pack_into("<I", data, NETWORK_VERSION_OFFSET, version + 1)
    film = work / "other_version.film"
    film.write_bytes(bytes(data))
    played = _play(work, film, "play-other-version", seconds=60)
    assert "was made by another build" in played, played[-3000:]
    assert "theater: could not play" in played, played[-3000:]
    assert not re.search(r"assert|exception|segmentation fault", played, re.I), played[-3000:]


def test_a_damaged_film_plays_up_to_the_damage(recorded):
    work, films = recorded
    data = bytearray(films["client"][1].read_bytes())
    # (bytes of its middle changed: a chunk there does not unpack, or unpacks to records that are not records)
    middle = len(data) // 2
    data[middle:middle + 64] = bytes(255 - byte for byte in data[middle:middle + 64])
    film = work / "damaged.film"
    film.write_bytes(bytes(data))
    played = _play(work, film, "play-damaged", seconds=120)
    assert "theater: the film is over" in played, played[-3000:]
    assert re.search(r"theater: the film('s chunk| has a damaged record) at tick \d+ (is damaged; )?", played), \
        played[-3000:]
    assert not re.search(r"assert|exception|segmentation fault", played, re.I), played[-3000:]
    ticks = int(re.search(r"the film is over \((\d+) of", played).group(1))
    assert 0 < ticks < 30 * 30, ticks
