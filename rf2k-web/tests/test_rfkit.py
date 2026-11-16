"""The codec, on bodies from the real amp and from FlexWeb's tests."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import rfkit  # noqa: E402

POWER = (b'{"temperature":{"value":41.5,"unit":"\xc2\xb0C"},"voltage":{"value":50.2,"unit":"V"},'
         b'"current":{"value":12.3,"unit":"A"},"forward":{"value":1200,"max_value":1500,"unit":"W"},'
         b'"reflected":{"value":30,"max_value":300,"unit":"W"},"swr":{"value":1.4,"max_value":10.0,"unit":""}}')


class Parsers(unittest.TestCase):
    def test_info(self):
        i = rfkit.parse_info(b'{"device":"RFKIT","software_version":{"GUI":12,"controller":34},'
                             b'"custom_device_name":"Shack amp"}')
        self.assertEqual(i, {"device": "RFKIT", "custom_name": "Shack amp", "gui_version": 12,
                             "controller_version": 34})
        self.assertIsNone(rfkit.parse_info(b'{"name":"router"}'))
        self.assertIsNone(rfkit.parse_info(b"<html>"))

    def test_power(self):
        p = rfkit.parse_power(POWER)
        self.assertEqual(p["forward"], {"value": 1200, "unit": "W", "max": 1500})
        self.assertEqual(p["swr"]["value"], 1.4)
        self.assertAlmostEqual(rfkit.temperature_celsius(p["temperature"]), 41.5)
        # a missing measurement: the whole body is ignored, not half-filled
        self.assertIsNone(rfkit.parse_power(b'{"forward":{"value":1,"unit":"W"}}'))
        self.assertIsNone(rfkit.parse_power(POWER.replace(b'"value":1200', b'"value":"1200"')))
        self.assertIsNone(rfkit.parse_power(POWER.replace(b'"value":1200', b'"value":true')))

    def test_body_cap(self):
        self.assertIsNone(rfkit.parse_info(b'{"device":"RFKIT","x":"' + b"a" * 70000 + b'"}'))

    def test_data(self):
        d = rfkit.parse_data(b'{"band":{"value":20,"unit":"m"},"frequency":{"value":14200,"unit":"kHz"},"status":""}')
        self.assertEqual(rfkit.format_band(d["band"]), "20 m")
        self.assertEqual(rfkit.format_frequency(d["frequency"]), "14.200 MHz")
        self.assertEqual(d["status"], "")
        self.assertEqual(rfkit.format_frequency({"value": 7074000, "unit": "Hz"}), "7.074 MHz")
        self.assertEqual(rfkit.format_frequency({"value": 28, "unit": "MHz"}), "28.000 MHz")
        self.assertEqual(rfkit.format_frequency({"value": 5, "unit": "furlongs"}), "5 furlongs")

    def test_fahrenheit(self):
        self.assertAlmostEqual(rfkit.temperature_celsius({"value": 140.0, "unit": "°F"}), 60.0)

    def test_tuner(self):
        t = rfkit.parse_tuner(b'{"mode":"AUTO","setup":"CL","L":{"value":755,"unit":"nH"},"C":{"value":439,"unit":"pF"},'
                              b'"tuned_frequency":{"value":14200,"unit":"kHz"},"segment_size":{"value":50,"unit":"kHz"}}')
        self.assertEqual((t["mode"], t["setup"], t["L"]["value"]), ("AUTO", "CL", 755))
        cases = {
            ("AUTO", "CL"): "ATU IN", ("AUTO_TUNING_FROM_AUTO", "CL"): "ATU IN",
            ("MANUAL", "LC"): "ATU MAN", ("MANUAL", "BYPASS"): "ATU MAN BYP",
            ("BYPASS", "BYPASS"): "ATU BYP", ("OFF", ""): "ATU OFF",
            ("AUTO", "NOT TUNED"): "ATU NO TUNE", ("MANUAL", "NOT TUNED"): "ATU NO TUNE",
        }
        for (mode, setup), pill in cases.items():
            t = rfkit.parse_tuner(('{"mode":"%s","setup":"%s"}' % (mode, setup)).encode())
            self.assertEqual(rfkit.tuner_pill(t), pill, (mode, setup))
        self.assertEqual(rfkit.parse_tuner(b'{"mode":false}')["mode"], "OFF")
        self.assertTrue(rfkit.tune_allowed(rfkit.parse_tuner(b'{"mode":"MANUAL"}')))
        self.assertFalse(rfkit.tune_allowed(rfkit.parse_tuner(b'{"mode":"AUTO_TUNING_FROM_AUTO"}')))
        self.assertFalse(rfkit.tune_allowed(rfkit.parse_tuner(b'{"mode":"BYPASS"}')))
        self.assertTrue(rfkit.tuner_pill_alert("ATU NO TUNE"))
        self.assertTrue(rfkit.tuner_pill_in_use("ATU MAN"))
        self.assertFalse(rfkit.tuner_pill_in_use("ATU MAN BYP"))

    def test_operate_interface(self):
        self.assertEqual(rfkit.parse_operate_mode(b'{"operate_mode":"OPERATE"}'), "OPERATE")
        self.assertIsNone(rfkit.parse_operate_mode(b'{"operate_mode":"ON"}'))
        self.assertEqual(rfkit.parse_interface(b'{"operational_interface":"TCI","error":"link lost"}'),
                         {"interface": "TCI", "error": "link lost"})
        self.assertIsNone(rfkit.parse_interface(b'{"operational_interface":"USB"}'))

    def test_antennas(self):
        obj = (b'{"antennas":[{"type":"INTERNAL","number":1,"state":"ACTIVE"},'
               b'{"type":"INTERNAL","number":2,"state":"AVAILABLE"},{"type":"EXTERNAL","state":"AVAILABLE"},'
               b'{"type":"INTERNAL","number":9,"state":"AVAILABLE"},{"type":"INTERNAL","number":3,"state":"WEIRD"}]}')
        a = rfkit.parse_antennas(obj)
        self.assertEqual(a, [{"external": False, "num": 1, "state": "active"},
                             {"external": False, "num": 2, "state": "available"},
                             {"external": True, "num": 0, "state": "available"}])
        bare = rfkit.parse_antennas(b'[{"type":"EXTERNAL","number":5,"state":"ACTIVE"}]')
        self.assertEqual(bare, [{"external": True, "num": 0, "state": "active", "port": 5}])
        self.assertIsNone(rfkit.parse_antennas(b'{"x":1}'))

    def test_bodies(self):
        self.assertEqual(rfkit.operate_body(False), b'{"operate_mode": "STANDBY"}')
        self.assertEqual(rfkit.antenna_body(3), b'{"type": "INTERNAL", "number": 3}')
        for bad in (0, 5, "1", True, None):
            self.assertIsNone(rfkit.antenna_body(bad))

    def test_tune_probe(self):
        for code, want in ((200, "supported"), (400, "supported"), (404, "unsupported"),
                           (405, "unsupported"), (501, "unsupported"), (409, "unknown"), (500, "unknown")):
            self.assertEqual(rfkit.classify_tune_probe(code), want, code)

    def test_tune_drive(self):
        def d(w, unit="W"):
            return rfkit.tune_drive({"forward": {"value": w, "unit": unit}})
        self.assertEqual([d(3), d(4), d(9), d(10), d(39), d(40)],
                         ["outside", "low", "low", "good", "good", "outside"])
        self.assertEqual(d(10, "dBm"), "unknown")


if __name__ == "__main__":
    unittest.main()
