import csv
import math
import argparse
import matplotlib.pyplot as plt

def read_rank_freq(path, max_points=None):
    ranks = []
    freqs = []
    with open(path, "r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            rank = int(row["rank"])
            freq = int(row["freq"])
            ranks.append(rank)
            freqs.append(freq)
            if max_points is not None and len(ranks) >= max_points:
                break
    return ranks, freqs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True, help="zipf_rank_freq.csv")
    ap.add_argument("--out", dest="out", required=True, help="output png path")
    ap.add_argument("--max", dest="max_points", type=int, default=200000, help="max points to plot")
    args = ap.parse_args()

    ranks, freqs = read_rank_freq(args.inp, args.max_points)
    if not ranks:
        raise SystemExit("No data")

    C = freqs[0]  # Zipf: f(r)=C/r

    zipf_line = [C / r for r in ranks]

    plt.figure()
    plt.loglog(ranks, freqs, marker=".", linestyle="none")
    plt.loglog(ranks, zipf_line)
    plt.xlabel("Rank (log)")
    plt.ylabel("Frequency (log)")
    plt.title("Zipf's law: term frequency vs rank (log-log)")
    plt.tight_layout()
    plt.savefig(args.out, dpi=200)

if __name__ == "__main__":
    main()
