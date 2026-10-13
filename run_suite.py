"""Randomized repeated paired trials; set interval from a separate baseline pilot."""
import argparse
import csv
import pathlib
import random
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--events', type=int, default=1000000)
p.add_argument('--interval-ns', type=int, required=True)
p.add_argument('--repeats', type=int, default=5)
p.add_argument('--out', default='results/suite')
args = p.parse_args()
if args.repeats < 1: p.error('repeats must be positive')
root = pathlib.Path(__file__).resolve().parent
out = pathlib.Path(args.out).resolve()
out.mkdir(parents=True, exist_ok=True)
rng = random.Random(42)
all_rows = []
for repeat in range(args.repeats):
    modes = ['baseline', 'allocate', 'grow', 'log-buffered', 'log-flush', 'pause']
    rng.shuffle(modes)
    for mode in modes:
        prefix = out / f'{repeat:02d}-{mode}'
        subprocess.run([str(root/'tail_bench'), '--mode', mode, '--events', str(args.events),
                        '--interval-ns', str(args.interval_ns), '--output', str(prefix)], check=True)
        with open(str(prefix)+'.summary.csv') as f:
            all_rows.extend(dict(repeat=repeat, **r) for r in csv.DictReader(f))
with open(out/'all-summary.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(all_rows[0]))
    w.writeheader()
    w.writerows(all_rows)
print(out/'all-summary.csv')
