#!/usr/bin/env python
"""Plots for the C++ benchmark output. The hot path stays entirely in C++
(this script never touches an order book) -- it just visualizes
results/latency_benchmarks.csv and results/throughput.csv, which is
exactly the realistic split of labor for this kind of project: C++ for
anything latency-sensitive, Python for analyzing/reporting on the results.

    python scripts/plot_results.py
"""

import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

RESULTS_DIR = os.path.join(os.path.dirname(__file__), "..", "results")


def plot_latency(df: pd.DataFrame, path: str) -> None:
    ops = ["add", "cancel", "cross"]
    labels = {"add": "Add (non-crossing)", "cancel": "Cancel", "cross": "Crossing (marketable)"}
    metrics = ["p50_ns", "p90_ns", "p99_ns", "p999_ns"]
    metric_labels = ["p50", "p90", "p99", "p99.9"]

    fig, axes = plt.subplots(1, len(ops), figsize=(15, 4.5), sharey=True)
    x = np.arange(len(metrics))
    width = 0.35
    for ax, op in zip(axes, ops):
        fast = df[df["benchmark"] == f"{op}_fast"].iloc[0]
        naive = df[df["benchmark"] == f"{op}_naive"].iloc[0]
        ax.bar(x - width / 2, [fast[m] for m in metrics], width, label="OrderBook (array+pool)", color="#1f6f8b")
        ax.bar(x + width / 2, [naive[m] for m in metrics], width, label="NaiveOrderBook (map+heap)", color="#c1440e")
        ax.set_xticks(x)
        ax.set_xticklabels(metric_labels)
        ax.set_yscale("log")
        ax.set_title(labels[op])
        ax.grid(alpha=0.3, axis="y")
    axes[0].set_ylabel("Latency (ns, log scale)")
    axes[0].legend(fontsize=8, loc="upper left")
    fig.suptitle("Order book operation latency: percentiles, not means")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def plot_throughput(df: pd.DataFrame, path: str) -> None:
    fig, ax = plt.subplots(figsize=(6, 4.5))
    labels = {"fast": "OrderBook\n(array+pool)", "naive": "NaiveOrderBook\n(map+heap)"}
    colors = {"fast": "#1f6f8b", "naive": "#c1440e"}
    x = [labels[b] for b in df["book"]]
    y = df["ops_per_sec"] / 1e6
    ax.bar(x, y, color=[colors[b] for b in df["book"]])
    ax.set_ylabel("Throughput (million ops/sec)")
    ax.set_title("Mixed-workload throughput")
    ax.grid(alpha=0.3, axis="y")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    latency_df = pd.read_csv(os.path.join(RESULTS_DIR, "latency_benchmarks.csv"))
    throughput_df = pd.read_csv(os.path.join(RESULTS_DIR, "throughput.csv"))

    plot_latency(latency_df, os.path.join(RESULTS_DIR, "latency_comparison.png"))
    plot_throughput(throughput_df, os.path.join(RESULTS_DIR, "throughput_comparison.png"))
    print(f"Wrote plots to {RESULTS_DIR}/")


if __name__ == "__main__":
    main()
