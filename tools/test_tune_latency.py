import unittest

from tools.tune_latency import parse_log_lines, summarize


class TuneLatencyTest(unittest.TestCase):
    def test_parses_text_and_json_and_summarizes_each_tune_stage(self):
        lines = [
            '[123ms] [ts] [INFO] Tune timing: system=0x00000008 freq_khz=473143 set_ms=23 lock_ms=145 first_ts_ms=240\n',
            '{"t_ms":456,"level":"info","cat":"ts","dev":"","msg":"Tune timing: system=0x00000008 freq_khz=473143 set_ms=30 lock_ms=160 first_ts_ms=280"}\n',
            '[789ms] [ts] [INFO] Tune timing: system=0x00000008 freq_khz=473143 set_ms=1 lock_ms=bad first_ts_ms=3\n',
            '[790ms] [server] [INFO] unrelated message\n',
        ]

        samples = parse_log_lines(lines)
        result = summarize(samples)

        self.assertEqual(len(samples), 2)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0].system, 8)
        self.assertEqual(result[0].frequency_khz, 473143)
        self.assertEqual(result[0].count, 2)
        self.assertEqual(result[0].set_ms.median, 26.5)
        self.assertEqual(result[0].set_ms.p95, 30)
        self.assertEqual(result[0].lock_ms.median, 152.5)
        self.assertEqual(result[0].first_ts_ms.median, 260)
        self.assertEqual(result[0].first_ts_ms.maximum, 280)

    def test_keeps_different_tuned_frequencies_separate(self):
        lines = [
            '[1ms] [ts] [INFO] Tune timing: system=8 freq_khz=473143 set_ms=1 lock_ms=2 first_ts_ms=3',
            '[2ms] [ts] [INFO] Tune timing: system=8 freq_khz=479143 set_ms=4 lock_ms=5 first_ts_ms=6',
        ]

        result = summarize(parse_log_lines(lines))

        self.assertEqual([(s.frequency_khz, s.count) for s in result], [(473143, 1), (479143, 1)])


if __name__ == '__main__':
    unittest.main()
