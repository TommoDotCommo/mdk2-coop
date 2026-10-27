import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import script_census  # noqa: E402
import script_diff  # noqa: E402

SCRIPT = """
r2_dog01.OnDie = function(gob) end
mdkSetLuaEvent(l1_r3, "OnEnterRoom", Level.Room3)
-- l1_r1.ch1.OnDie = l1_r1.ConeheadOnDie
if (x == y) then x.OnTimer = nil end
omGobSetTimer(gob, 4)  mdkSetShatterTrigger(a, b)
"""


class Census(unittest.TestCase):
    def test_counts_handlers_not_comments_or_comparisons(self):
        events, other = script_census.census({"level1.lua": SCRIPT, "l2.lua": "a.OnDie = f\nb.OnDie == c"})
        self.assertEqual(events["OnDie"], {"level1.lua": 1, "l2.lua": 1})
        self.assertEqual(events["OnEnterRoom"], {"level1.lua": 1})
        self.assertEqual(events["OnTimer"], {"level1.lua": 1})
        self.assertEqual(other["timers (omGobSetTimer, mdkSetTimer)"], 1)
        self.assertEqual(other["shatter triggers"], 1)

    def test_table_lists_the_engine_events_the_trace_names(self):
        table = script_census.load_table()
        source = (ROOT / "bridge/scripttrace.c").read_text()
        names = re.findall(r'"(On\w+)"', source.split("event_name[] = {")[1].split("};")[0])
        for i, name in enumerate(names):
            self.assertEqual(table[name][0], i, "%s: table id vs bridge/scripttrace.c" % name)
        self.assertEqual(len(names), 19)

    def test_unknown_event_is_reported(self):
        table = script_census.load_table()
        events, other = script_census.census({"x.lua": "g.OnBrandNew = f"})
        import io
        unlisted, _ = script_census.report(events, other, table, io.StringIO())
        self.assertEqual(unlisted, ["OnBrandNew"])

    def test_bad_table_lines_are_errors(self):
        for bad in ("0 | OnDie | sometimes | - | x", "0 | OnDie | synced | -", "0 | Die | synced | - | x"):
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "t.txt"
                p.write_text(bad + "\n")
                with self.assertRaises(ValueError):
                    script_census.load_table(p)


class Triggers(unittest.TestCase):
    CODE = """
r8.OnUpdate = function(gob)
    if (Save.railnum == nil and mdkGobDistancePoint(mdkGetPlayerGob(), "r8_center") < 40) then
        Level.StartBoss()
        for i = 1, 3 do Level.Spawn(i) end
    end
end
l1_r2.OnUpdate = function(gob)
    omGobSetTimer(gob, 1)
end
mdkSetLuaEvent(l1_r3, "OnTimer", function(gob) local p = mdkGetPlayerGob() end)
l1_r4.OnDie = Level.Named
"""

    def test_inline_handlers_with_their_bodies(self):
        got = [(o, e) for o, e, _ in script_census.handler_bodies(self.CODE)]
        self.assertEqual(got, [("r8", "OnUpdate"), ("l1_r2", "OnUpdate"), ("l1_r3", "OnTimer")])
        body = script_census.handler_bodies(self.CODE)[0][2]
        self.assertIn("StartBoss", body)
        self.assertNotIn("omGobSetTimer", body, "the body stops at its own end, nesting counted")

    def test_places_to_send_a_player(self):
        self.assertEqual(script_census.triggers(self.CODE), ["r8", "r8_center", "l1_r3"])


class Proximity(unittest.TestCase):
    def test_only_looking_at_the_player(self):
        look = 'if (mdkGobDistancePoint(mdkGetPlayerGob(), "r8_center") < 40) then Save.x = 1 end'
        self.assertTrue(script_census.reads_player_only_to_look(look))
        alias = 'local p = mdkGetPlayerGob()\n if (mdkGobDistance(gob, p) < 5 and p.position.z > 3) then Save.x = 1 end'
        self.assertTrue(script_census.reads_player_only_to_look(alias))

    def test_acting_on_the_player_does_not_count(self):
        for body in ('mdkDealDamage(nil, mdkGetPlayerGob(), 5, 1, -1)',
                     'local p = mdkGetPlayerGob() mdkGobSetPosition(p, "x")',
                     'mdkDocGiveItem(mdkGetPlayerGob(), OBJ_X)',
                     'Save.p = mdkGetPlayerGob()',
                     'if (mdkGobDistance(mdkGetPlayerGob(), gob) < 4) then mdkKurtEnterNormalMode(mdkGetPlayerGob()) end',
                     'omGobSetTimer(gob, 1)'):
            self.assertFalse(script_census.reads_player_only_to_look(body), body)

    def test_table_for_the_lua_file(self):
        scripts = {"level7.lua": 'r8.OnUpdate = function(gob)\n if (mdkGobDistancePoint(mdkGetPlayerGob(), "c") < 40) then Level.Go() end\nend\n'
                                 'door.OnUpdate = function(gob)\n if (mdkGobDistance(mdkGetPlayerGob(), gob) < 4) then x = 1 end\nend\n'
                                 'l7r1.OnCollision = function(gob, t)\n if (mdkGobDistance(mdkGetPlayerGob(), gob) < 4) then x = 1 end\nend\n'
                                 'Level.Near = function(gob)\n if (mdkGobDistancePoint(mdkGetPlayerGob(), "d") < 9) then x = 1 end\nend\n'
                                 'l7r4_a.OnTimer = Level.Near\n'}
        prox = script_census.proximity(scripts)
        self.assertEqual(prox, {7: [("r8", "OnUpdate"), ("l7r4_a", "OnTimer")]})
        self.assertEqual(script_census.proximity_lua(prox),
                         'coop.nearest = {\n    [7] = { "r8", "OnUpdate", "l7r4_a", "OnTimer" },\n}')

    def test_the_table_in_the_lua_file_is_what_the_scripts_give(self):
        lua = (ROOT / "lua/coop_dev.lua").read_text()
        self.assertIn(script_census.proximity_lua({1: [("l1_r3spinners", "OnUpdate"), ("l1_r8", "OnUpdate")]}).split("\n")[1], lua)

    def test_inventory_pickups(self):
        scripts = {"level1.lua": 'Level.SG = function(gob, id)\n mdkKurtRemoveItem(mdkGetPlayerGob(), OBJ_GRENADEITEM, 4)\nend\n'
                                 'l1_r1.grenades.OnPickedUp = Level.SG\n'
                                 'l1_r7.mortars.OnPickedUp = function(gob, id) l1_r7.x = nil end\n'}
        got = script_census.inventory_pickups(scripts)
        self.assertEqual([(f, o) for f, o, _ in got], [("level1.lua", "l1_r1.grenades")])


class Known(unittest.TestCase):
    def rows(self, text):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "k.txt"
            p.write_text(text)
            return script_diff.load_known(p)

    def test_wildcards_levels_and_sides(self):
        known = self.rows("8 | OnTimer | l8r1_cc* | host only | reviewed | cosmetic\n* | OnPickedUp | mortars | * | bug | open\n")
        self.assertEqual(script_diff.known_status((8, "OnTimer", "l8r1_cc07", "host only"), known), ("reviewed", "cosmetic"))
        self.assertIsNone(script_diff.known_status((8, "OnTimer", "l8r1_cc07", "client only"), known))
        self.assertIsNone(script_diff.known_status((9, "OnTimer", "l8r1_cc07", "host only"), known))
        self.assertEqual(script_diff.known_status((3, "OnPickedUp", "mortars", "client only"), known), ("bug", "open"))

    def test_the_shipped_file_loads(self):
        self.assertGreaterEqual(len(script_diff.load_known()), 2)

    def test_bad_lines_are_errors(self):
        for bad in ("8 | OnTimer | x | host only | fine | n", "8 | OnTimer | x | sideways | bug | n", "8 | OnTimer | x"):
            with self.assertRaises(ValueError):
                self.rows(bad + "\n")


class Diff(unittest.TestCase):
    HOST = ("script event L1: OnDie r2_dog01\nscript event L1: OnUpdate coop_p2 (mirror)\n"
            "script event L1: OnCreate (unnamed)\nscript event L1: OnTimer n1a2b3c\n"
            "12 1793000000000 abcdef0123456789 script event L2: OnEnterRoom l2_r1\n")
    CLIENT = "script event L1: OnCreate l1_r2\nscript event L2: OnEnterRoom l2_r1\n"

    def test_parse_skips_puppets_unnamed_and_network_names_and_reads_session_lines(self):
        self.assertEqual(script_diff.parse(self.HOST),
                         {1: {("OnDie", "r2_dog01")}, 2: {("OnEnterRoom", "l2_r1")}})

    def test_diff_names_the_side(self):
        rows = script_diff.diff(script_diff.parse(self.HOST), script_diff.parse(self.CLIENT))
        self.assertEqual(rows, [(1, "OnDie", "r2_dog01", "host only"), (1, "OnCreate", "l1_r2", "client only")])

    def test_a_level_only_one_side_saw_is_not_compared(self):
        self.assertEqual(script_diff.diff({3: {("OnDie", "x")}}, {4: set()}), [])


if __name__ == "__main__":
    unittest.main()
