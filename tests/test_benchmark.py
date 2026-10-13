"""Correctness/invariant checks, never performance thresholds."""
import csv
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / 'tail_bench'

class BenchmarkTest(unittest.TestCase):
    def test_self_test(self):
        subprocess.run([str(BIN), '--self-test'], check=True)

    def test_modes_preserve_work_and_timing_identities(self):
        checksums = set()
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ['baseline', 'allocate', 'grow', 'log-buffered', 'log-flush', 'pause']:
                prefix = pathlib.Path(tmp) / mode
                subprocess.run([str(BIN), '--mode', mode, '--events', '16384',
                                '--interval-ns', '1000', '--output', str(prefix)], check=True)
                with open(str(prefix)+'.samples.csv') as f:
                    rows = list(csv.DictReader(f))
                self.assertEqual(len(rows), 16384)
                for row in rows:
                    i = int(row['event'])
                    self.assertEqual(int(row['scheduled_ns']), i * 1000)
                    self.assertGreaterEqual(int(row['start_ns']), int(row['scheduled_ns']))
                    self.assertEqual(int(row['end_ns'])-int(row['start_ns']), int(row['service_ns']))
                    self.assertEqual(int(row['response_ns']), int(row['service_ns'])+int(row['wait_ns']))
                growth = sum(int(r['growth']) for r in rows)
                self.assertGreater(growth, 0) if mode == 'grow' else self.assertEqual(growth, 0)
                self.assertEqual(sum(int(r['pause']) for r in rows), 1 if mode == 'pause' else 0)
                with open(str(prefix)+'.summary.csv') as f:
                    summary = list(csv.DictReader(f))
                checksums.add(summary[0]['checksum'])
                for s in summary:
                    self.assertLessEqual(int(s['p50_ns']), int(s['p99_ns']))
                    self.assertLessEqual(int(s['p99_ns']), int(s['p999_ns']))
                    self.assertLessEqual(int(s['p999_ns']), int(s['max_ns']))
                if mode.startswith('log-'):
                    self.assertEqual(pathlib.Path(str(prefix)+'.bin').stat().st_size, 16384*8)
        self.assertEqual(len(checksums), 1)

    def test_unpaced_and_invalid_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = str(pathlib.Path(tmp)/'unpaced')
            subprocess.run([str(BIN), '--events', '100', '--interval-ns', '0', '--output', prefix], check=True)
            with open(prefix+'.samples.csv') as f:
                for row in csv.DictReader(f):
                    self.assertEqual(row['wait_ns'], '0')
                    self.assertEqual(row['service_ns'], row['response_ns'])
        for args in [['--events', '0'], ['--mode', 'wrong'], ['--interval-ns', '-1']]:
            self.assertNotEqual(subprocess.run([str(BIN), *args], capture_output=True).returncode, 0)

if __name__ == '__main__': unittest.main()
