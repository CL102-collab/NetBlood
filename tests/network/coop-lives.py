#!/usr/bin/env python3
"""Exercise co-op lives, teammate revival and fresh-map wipes with real clients.

Requires a built game, Blood 1.21 assets and X11 (an Xvfb wrapper is suitable).
Ordinary movement, weapons and Use perform every death, revival and respawn.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from cinematics import capture_window, focus_window, free_port
from run import Failure, Game as BaseGame, build_driver, inspect_demo, play_demo, records, require
from scoreboard import demo_scoreboards


class Game(BaseGame):
    def command_text(self, commands, timeout=10):
        if not self.dedicated and self.process.poll() is None:
            focus_window(self)
        return super().command_text(commands, timeout)


def status(game):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    sessions, rules = records(text, "NET status "), records(text, "NET rules ")
    require(sessions and rules, f"{game.name}: missing session/rule diagnostics")
    views, worlds = records(text, "NET view "), records(text, "NET world ")
    return {"session": sessions[-1], "rules": rules[-1],
            "players": {row["slot"]: row for row in records(text, "NET player ")},
            "scoreboard": {row["slot"]: row for row in records(text, "NET scoreboard ")},
            "view": views[-1] if views else None, "world": worlds[-1] if worlds else None}


def expect(game, predicate, description, timeout=45):
    deadline, state = time.monotonic() + timeout, None
    while time.monotonic() < deadline:
        state = status(game)
        if predicate(state):
            return state
        time.sleep(0.1)
    raise Failure(f"{game.name}: timed out waiting for {description}; last state={state}")


def row(state, slot):
    require(str(slot) in state["players"], f"missing player slot {slot}: {state}")
    return state["players"][str(slot)]


def alive(player):
    return int(player["health"]) > 0 and player["downed"] == "0" and player["spectator"] == "0"


def press(game, scan, seconds=0.2):
    focus_window(game)
    game.key(scan, True)
    try:
        time.sleep(seconds)
    finally:
        game.key(scan, False)
    time.sleep(0.2)


def approach_downed(host, rescuer, rescuer_slot, target_slot, timeout=20):
    """Walk the east-facing rescuer into Use range after physical TNT push."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = status(host)
        source, target = row(state, rescuer_slot), row(state, target_slot)
        dx, dy = int(target["x"]) - int(source["x"]), int(target["y"]) - int(source["y"])
        if max(abs(dx), abs(dy)) + min(abs(dx), abs(dy)) // 2 < 650:
            return state
        scan = (26 if dx > 0 else 22) if abs(dx) >= abs(dy) else (7 if dy > 0 else 4)
        press(rescuer, scan, 0.1)  # W/S along X, D/A along Y.
    raise Failure("rescuer did not reach the downed teammate with ordinary movement")


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--skip-screenshots", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-coop-lives-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Co-op lives evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    spec = importlib.util.spec_from_file_location("coop_lives_fixture", Path(__file__).with_name("coop-lives-fixture.py"))
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    fixtures = {"NBLIVES1.MAP": fixture.lives_map(), "NBLIVES2.MAP": fixture.lives_map(1)}
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "phases": {}, "screenshots": {}, "failures": []}
    games = []

    def server(name, limit=None):
        port = free_port()
        arguments = ["-dedicated", "-port", str(port), "-gametype", "coop", "-maxplayers", "2",
                     "-map", "NBLIVES1.MAP", "-losekeys", "off", "-weaponsstay", "off"]
        if limit is not None:
            arguments += ["-lives", str(limit)]
        game = Game(name, args, root, library, port, dedicated=True,
                    launch_arguments=arguments, initial_files=fixtures)
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated co-op server", args.timeout)
        require(status(game)["rules"]["coop_lives"] == str(limit if limit is not None else 4),
                "server did not apply its configured/default co-op life limit")
        return game, port

    def client(name, port, player_name):
        game = Game(name, args, root, library, port, initial_files=fixtures,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), "-pname", player_name])
        games.append(game)
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "authoritative client snapshot", args.timeout)
        slot = int(records(game.text(), "NET welcome ")[-1]["slot"])
        game.command_text('bind H "gamefunc_Open"\nbind S "gamefunc_Move_Backward"\n'
                          'bind A "gamefunc_Strafe_Left"\nbind D "gamefunc_Strafe_Right"\n'
                          'bind X "gamefunc_Turn_Around"\nbind 3 "gamefunc_Weapon_3"\n'
                          'bind C "gamefunc_Aim_Down"\nbind Z "gamefunc_Aim_Center"\n'
                          'bind 6 "gamefunc_Weapon_6"\nbind F11 "gamefunc_Show_Scoreboard"')
        return game, slot

    def budgets(host, expected, clients=()):
        def matches(state):
            return all(str(slot) in state["players"] and row(state, slot)["lives"] == str(lives)
                       for slot, lives in expected.items())
        state = expect(host, matches, f"life budgets {expected}", args.timeout)
        for game in clients:
            observed = expect(game, lambda current: matches(current) and
                              all(current["scoreboard"].get(str(slot), {}).get("lives") == str(lives)
                                  for slot, lives in expected.items()),
                              f"installed/scoreboard life budgets {expected}", args.timeout)
            require(observed["rules"]["coop_lives"] == state["rules"]["coop_lives"],
                    "client did not install the server life limit")
        return state

    def screenshot(game, name, scoreboard=False):
        if args.skip_screenshots:
            return
        focus_window(game)
        if scoreboard:
            game.key(68, True)  # F11, explicitly bound above.
        try:
            time.sleep(0.4)
            result["screenshots"][name] = capture_window(game, root / (name + ".png"))
        finally:
            if scoreboard:
                game.key(68, False)

    def move_until(host, game, slot, scan, predicate, description):
        focus_window(game)
        game.key(scan, True)
        try:
            return expect(host, lambda state: predicate(row(state, slot)), description, args.timeout)
        finally:
            game.key(scan, False)
            time.sleep(0.2)

    def fall(host, game, slot):
        before = status(host)
        require(alive(row(before, slot)), f"slot {slot}: fatal fall must start alive")
        state = move_until(host, game, slot, 26, lambda player: player["downed"] == "1", "ordinary fall into kneeling state")
        require(row(state, slot)["lives"] == row(before, slot)["lives"], "lethal damage consumed a life before giving up")
        require(state["session"]["map_generation"] == before["session"]["map_generation"], "downing restarted the map")
        return {"before": before, "downed": state}

    def give_up(host, game, slot, reset_hold=False, wipe=False):
        before = status(host)
        previous = row(before, slot)
        require(previous["downed"] == "1", "give up requires a downed player")
        remaining = int(previous["lives"]) - 1
        if reset_hold:
            focus_window(game)
            game.key(11, True)
            try:
                halfway = expect(host, lambda state: int(row(state, slot)["giveup"]) >= 30,
                                  "partial give-up progress", args.timeout)
                require(row(halfway, slot)["downed"] == "1" and row(halfway, slot)["lives"] == previous["lives"],
                        "partial hold consumed a life")
            finally:
                game.key(11, False)
            released = expect(host, lambda state: row(state, slot)["giveup"] == "0",
                              "released give-up progress reset", args.timeout)
            require(row(released, slot)["downed"] == "1", "releasing a partial hold finalized death")
        count = len(records(host.text(), "NET coop death "))
        focus_window(game)
        start = time.monotonic()
        game.key(11, True)
        try:
            host.wait(lambda text: len(records(text, "NET coop death ")) > count,
                      "three-second give up", args.timeout)
        finally:
            game.key(11, False)
        elapsed = time.monotonic() - start
        require(elapsed >= 2.8, "give-up completed before a fresh three-second hold")
        deaths = records(host.text(), "NET coop death ")
        require(len(deaths) == count + 1 and deaths[-1]["slot"] == str(slot) and deaths[-1]["lives"] == str(remaining),
                f"final death did not consume exactly one life: {deaths[count:]}")
        time.sleep(0.3)
        if wipe:
            after = expect(host, lambda state: int(state["session"]["map_generation"]) > int(before["session"]["map_generation"]),
                           "automatic fresh level restart", args.timeout)
        else:
            after = budgets(host, {slot: remaining}, [game])
            require(after["session"]["map_generation"] == before["session"]["map_generation"],
                    "map restarted while another connected player retained a life")
        return {"before": before, "after": after, "hold_seconds": elapsed, "death": deaths[-1]}

    def respawn(host, game, slot, remaining):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            current = status(host)
            if alive(row(current, slot)):
                require(row(current, slot)["lives"] == str(remaining), "respawn changed remaining life budget")
                return current
            press(game, 11)
        raise Failure(f"slot {slot}: ordinary Open did not respawn a player with {remaining} lives")

    try:
        host, port = server("default-server")
        first, first_slot = client("client-1", port, "Lives_A")
        require(first_slot == 0, "first fixture client must use start zero")
        initial = budgets(host, {first_slot: 4}, [first])
        require(initial["world"] and initial["world"]["monsters_alive"] == "1", "fixture monster missing")
        require(int(initial["world"]["pickups"]) == len(fixture.PICKUP_TYPES), "fixture pickups missing")
        pristine = {field: row(initial, first_slot)[field] for field in ("ammo_sum", "armor_sum", "keys_mask", "weapons_mask", "pack_sum")}
        require(all(int(pristine[field]) == 0 for field in ("ammo_sum", "armor_sum", "keys_mask", "pack_sum")),
                "fixture spawns overlap pickups")
        first.command_text("net_record coop-lives.nbpd")

        collected = move_until(host, first, first_slot, 22, lambda player: int(player["x"]) < -6200, "west fixture pickups")
        collected = status(host)
        require(all(int(row(collected, first_slot)[field]) > 0 for field in ("ammo_sum", "armor_sum", "keys_mask")) and
                row(collected, first_slot)["weapons_mask"] != pristine["weapons_mask"],
                "ordinary movement did not collect weapons/ammo/armor/key")
        require(int(collected["world"]["pickups"]) < int(initial["world"]["pickups"]), "pickups remained active after collection")
        press(first, 27)  # X: turn around, west toward the stationary tentacle.
        press(first, 32)  # 3: shotgun.
        time.sleep(1)
        press(first, 6, 0.1)  # C: aim at the short, floor-mounted tentacle.
        focus_window(first)
        first.key(13, True)  # J: ordinary Weapon_Fire.
        try:
            killed = expect(host, lambda state: state["world"]["monsters_alive"] == "0", "ordinary shotgun kill", args.timeout)
        finally:
            first.key(13, False)
        press(first, 29)  # Z: center aim again before returning along the platform.
        press(first, 27)  # Face east again.
        returned = move_until(host, first, first_slot, 26, lambda player: int(player["x"]) > -2300, "return to safe spawn platform")
        press(first, 35)  # 6: TNT. Holding a lit bundle causes ordinary self-damage.
        time.sleep(1)
        focus_window(first)
        first.key(13, True)
        try:
            downed = expect(host, lambda state: row(state, first_slot)["downed"] == "1", "TNT self-down on safe platform", args.timeout)
        finally:
            first.key(13, False)
        require(row(downed, first_slot)["lives"] == "4", "downing consumed a life")
        # Join after the blast so its physical push cannot move the rescuer out
        # of Use range, even though friendly fire is disabled.
        second, second_slot = client("late-client", port, "Lives_B")
        require(second_slot == 1, "second fixture client must use nearby start one")
        admitted = budgets(host, {first_slot: 4, second_slot: 4}, [first, second])
        require(row(admitted, first_slot)["downed"] == "1" and alive(row(admitted, second_slot)),
                "hot join did not preserve the downed player and admit a living rescuer")
        approach_downed(host, second, second_slot, first_slot)
        screenshot(first, "downed-hud")
        press(second, 11)
        revived = expect(host, lambda state: alive(row(state, first_slot)), "teammate Use revival", args.timeout)
        require(row(revived, first_slot)["health"] == str(50 << 4) and row(revived, first_slot)["lives"] == "4",
                "revival must restore 50 health without consuming a life")
        result["phases"]["revive-preserves-life"] = {"initial": initial, "collected": collected, "monster_killed": killed,
                                                        "returned": returned, "downed": downed, "revived": revived}
        print("PASS: pickups and monster changed normally; nearby teammate revives at 50 health without life loss", flush=True)

        deaths = []
        for remaining in (3, 2, 1, 0):
            down = fall(host, first, first_slot)
            death = give_up(host, first, first_slot, reset_hold=remaining == 3)
            entry = {"fall": down, "give_up": death}
            if remaining:
                entry["respawn"] = respawn(host, first, first_slot, remaining)
            deaths.append(entry)
        exhausted = budgets(host, {first_slot: 0, second_slot: 4}, [first, second])
        require(row(exhausted, first_slot)["spectator"] == "1", "exhausted player did not become a spectator")
        watched = expect(first, lambda state: state["view"].get("spectator") == "1" and
                         state["view"].get("target") == str(second_slot), "automatic surviving-teammate view", args.timeout)
        old_body = {field: row(exhausted, first_slot)[field] for field in ("x", "y", "z", "health")}
        press(first, 11, 0.5)
        press(first, 13, 0.5)
        press(first, 26, 0.5)
        denied = budgets(host, {first_slot: 0, second_slot: 4}, [first])
        require({field: row(denied, first_slot)[field] for field in old_body} == old_body,
                "exhausted input moved/fired/respawned the player")
        screenshot(first, "spectator-hud")
        screenshot(first, "spectator-scoreboard", scoreboard=True)
        first.command_text("net_stoprecord")
        demo = inspect_demo(first.path / "coop-lives.nbpd")
        metadata = demo_scoreboards(first.path / "coop-lives.nbpd")
        local_rows = [player for packet in metadata for player in packet["rows"] if player["slot"] == str(first_slot)]
        require(any(player["downed"] == "1" for player in local_rows) and
                any(0 < int(player["giveup"]) < 90 for player in local_rows) and
                any(player["lives"] == "0" and player["downed"] == "0" for player in local_rows),
                "packet recording omitted kneeling, partial give-up or exhausted metadata")
        demo["scoreboards"] = metadata
        result["phases"]["four-lives-three-respawns"] = {"deaths": deaths, "exhausted": exhausted,
                                                          "watching": watched, "denied": denied}
        print("PASS: four give-ups allow three respawns; exhausted players watch their teammate and cannot act", flush=True)

        first.stop()
        expect(host, lambda state: str(first_slot) not in state["players"], "exhausted player disconnect", args.timeout)
        rejoined, reused_slot = client("reconnected-client", port, "Lives_A")
        require(reused_slot == first_slot, "reconnection did not reuse the vacant slot")
        retained = budgets(host, {first_slot: 0, second_slot: 4}, [rejoined, second])
        require(row(retained, first_slot)["spectator"] == "1", "same-map reconnect bypassed exhaustion")
        result["phases"]["reconnect-retains-exhaustion"] = retained
        print("PASS: same-address/name reconnect retains exhausted budget", flush=True)

        second_deaths = []
        for remaining in (3, 2, 1, 0):
            down = fall(host, second, second_slot)
            death = give_up(host, second, second_slot, wipe=remaining == 0)
            entry = {"fall": down, "give_up": death}
            if remaining:
                # No player is living here, but one still owns another life.
                time.sleep(0.5)
                require(status(host)["session"]["map_generation"] == initial["session"]["map_generation"],
                        "server restarted merely because all players were temporarily dead")
                entry["respawn"] = respawn(host, second, second_slot, remaining)
            second_deaths.append(entry)
        fresh = budgets(host, {first_slot: 4, second_slot: 4}, [rejoined, second])
        require(int(fresh["session"]["map_generation"]) == int(initial["session"]["map_generation"]) + 1,
                "team exhaustion did not restart exactly once")
        generations = records(host.text(), "NET map generation=")
        require(generations[0]["crc"] == generations[-1]["crc"], "team exhaustion changed the map")
        require(fresh["world"] == initial["world"], "full wipe did not restore pristine pickups/monsters")
        for slot in (first_slot, second_slot):
            require(alive(row(fresh, slot)) and {field: row(fresh, slot)[field] for field in pristine} == pristine,
                    "team wipe retained a dead player, ammunition, armor or keys")
        result["phases"]["whole-team-fresh-restart"] = {"deaths": second_deaths, "fresh": fresh}
        print("PASS: exhausted team reloads the same map with fresh lives, inventory, keys, pickups and monster", flush=True)

        fall(host, second, second_slot)
        give_up(host, second, second_slot)
        budgets(host, {first_slot: 4, second_slot: 3})
        host.command_text("map NBLIVES2.MAP")
        transitioned = expect(host, lambda state: int(state["session"]["map_generation"]) == int(fresh["session"]["map_generation"]) + 1,
                              "normal new map", args.timeout)
        transitioned = budgets(host, {first_slot: 4, second_slot: 4}, [rejoined, second])
        require(records(host.text(), "NET map generation=")[-1]["crc"] != generations[0]["crc"], "second fixture did not load")
        result["phases"]["map-replenishes-lives"] = transitioned
        rejoined.stop()
        second.stop()
        host.stop()

        replay = Game("packet-playback", args, root, library, playback=True, initial_files=fixtures, launch_map="NBLIVES1")
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline playback setup", args.timeout)
        time.sleep(1)
        result["packet_demo"] = demo
        result["packet_playback"] = play_demo("packet-playback", demo, args, root, library, games, existing=replay)
        # play_demo records net_status and cleanly stops its game afterward.
        playback = {"players": {player["slot"]: player for player in records(replay.text(), "NET player ")},
                    "view": records(replay.text(), "NET view ")[-1]}
        require(row(playback, first_slot)["lives"] == "0" and row(playback, first_slot)["spectator"] == "1" and
                playback["view"].get("target") == str(second_slot), "packet playback lost exhausted spectator state")
        result["packet_playback"]["lives"] = playback
        print("PASS: packet recording/playback preserves exhausted lives and teammate viewing", flush=True)

        boundary, boundary_port = server("one-life-server", 1)
        single, single_slot = client("one-life-client", boundary_port, "One_Life")
        one_initial = budgets(boundary, {single_slot: 1}, [single])
        fall(boundary, single, single_slot)
        single.stop()
        empty = expect(boundary, lambda state: not state["players"], "downed last client disconnect", args.timeout)
        require(empty["session"]["map_generation"] == one_initial["session"]["map_generation"],
                "empty server treated a downed departure as a team wipe")
        single, returned_slot = client("one-life-reconnected", boundary_port, "One_Life")
        require(returned_slot == single_slot, "one-life reconnect changed fixture start")
        one_returned = budgets(boundary, {single_slot: 1}, [single])
        require(row(one_returned, single_slot)["downed"] == "1", "reconnecting escaped the downed state")
        one_death = give_up(boundary, single, single_slot, wipe=True)
        one_fresh = budgets(boundary, {single_slot: 1}, [single])
        require(alive(row(one_fresh, single_slot)) and
                int(one_fresh["session"]["map_generation"]) == int(one_initial["session"]["map_generation"]) + 1,
                "one-life limit allowed an extra respawn or failed to reload")
        result["phases"]["one-life-boundary"] = {"initial": one_initial, "reconnected_downed": one_returned,
                                                   "death": one_death, "fresh": one_fresh}
        single.stop()
        boundary.stop()

        unlimited, unlimited_port = server("unlimited-server", 0)
        legacy, legacy_slot = client("unlimited-client", unlimited_port, "Legacy_Coop")
        legacy_initial = budgets(unlimited, {legacy_slot: -1}, [legacy])
        legacy_deaths = []
        for _ in range(5):  # More deaths than the default finite life budget.
            dead = move_until(unlimited, legacy, legacy_slot, 26,
                              lambda player: player["health"] == "0", "legacy immediate death")
            require(row(dead, legacy_slot)["downed"] == "0" and row(dead, legacy_slot)["lives"] == "-1",
                    "unlimited legacy co-op entered kneeling or spent a finite life")
            restored = respawn(unlimited, legacy, legacy_slot, -1)
            require(restored["session"]["map_generation"] == legacy_initial["session"]["map_generation"],
                    "unlimited deaths caused an exhausted-team map reset")
            legacy_deaths.append({"dead": dead, "respawned": restored})
        require(not records(unlimited.text(), "NET coop downed ") and not records(unlimited.text(), "NET coop death "),
                "legacy unlimited mode ran limited-lives downing/death bookkeeping")
        result["phases"]["zero-lives-legacy-unlimited"] = {"initial": legacy_initial, "deaths": legacy_deaths}
        legacy.stop()
        unlimited.stop()
        print("PASS: lives zero retains immediate death and unlimited ordinary respawn", flush=True)

        installed = records(first.text(), "NET snapshot roundtrip ")
        require(len(installed) >= 30 and all(entry.get("equal") == "1" for entry in installed),
                "co-op changes failed exact authoritative snapshot installation")
        result["exact_snapshot_roundtrips"] = len(installed)
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"NET (?:apply snapshot|capture failed|invalid bootstrap|snapshot verification failed)|AddressSanitizer|runtime error:|Segmentation fault",
                         game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime/snapshot diagnostic")
            if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                result["failures"].append(f"{game.name}: missing headless device audit")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS" if result["passed"] else "FAIL", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
