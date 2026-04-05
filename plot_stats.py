#!/usr/bin/env python3

import csv
from datetime import datetime

import matplotlib.pyplot as plt


def to_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def to_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def is_number(value):
    try:
        float(value)
        return True
    except (TypeError, ValueError):
        return False


def detect_header(first_row, known_fields):
    normalized = {cell.strip().lower() for cell in first_row}
    return bool(normalized & known_fields)


def pick_value(row, index_map, names=(), indices=(), default=None):
    for name in names:
        index = index_map.get(name)
        if index is not None and index < len(row) and row[index] != "":
            return row[index]

    for index in indices:
        if index < len(row) and row[index] != "":
            return row[index]

    return default


def read_sender(path="sender_stats.csv"):
    times = []
    sent_packets = []

    with open(path, newline="", encoding="utf-8") as file:
        reader = csv.reader(file)
        rows = [row for row in reader if row]

        if not rows:
            return times, sent_packets

        sender_fields = {
            "kickoff_time",
            "kickoff_epoch",
            "total_time_sec",
            "data_packets_sent",
        }

        index_map = {}
        start_index = 0
        if detect_header(rows[0], sender_fields):
            index_map = {name.strip().lower(): idx for idx, name in enumerate(rows[0])}
            start_index = 1

        for row in rows[start_index:]:
            kickoff = to_int(
                pick_value(
                    row,
                    index_map,
                    names=("kickoff_time", "kickoff_epoch"),
                    indices=(0,),
                    default=0,
                )
            )
            elapsed = to_float(
                pick_value(
                    row,
                    index_map,
                    names=("total_time_sec",),
                    indices=(2,),
                    default=0.0,
                )
            )
            t = datetime.fromtimestamp(kickoff + elapsed)
            times.append(t)
            sent_packets.append(
                to_float(
                    pick_value(
                        row,
                        index_map,
                        names=("data_packets_sent",),
                        indices=(4, 3),
                        default=0.0,
                    )
                )
            )

    return times, sent_packets


def read_receiver(path="receiver_stats.csv"):
    times = []
    recv_stat = []
    loss_pct = []

    with open(path, newline="", encoding="utf-8") as file:
        reader = csv.reader(file)
        rows = [row for row in reader if row]

        if not rows:
            return times, recv_stat, loss_pct

        receiver_fields = {
            "kickoff_time",
            "kickoff_epoch",
            "end_time",
            "end_epoch",
            "total_time_sec",
            "unique_chunks_received",
            "files_received",
            "estimated_packets_lost",
        }

        index_map = {}
        start_index = 0
        if detect_header(rows[0], receiver_fields):
            index_map = {name.strip().lower(): idx for idx, name in enumerate(rows[0])}
            start_index = 1

        for row in rows[start_index:]:
            end_time = pick_value(
                row,
                index_map,
                names=("end_time", "end_epoch"),
                indices=(1,),
                default=None,
            )
            if end_time:
                t = datetime.fromtimestamp(to_int(end_time))
            else:
                kickoff = to_int(
                    pick_value(
                        row,
                        index_map,
                        names=("kickoff_time", "kickoff_epoch"),
                        indices=(0,),
                        default=0,
                    )
                )
                elapsed = to_float(
                    pick_value(
                        row,
                        index_map,
                        names=("total_time_sec",),
                        indices=(2,),
                        default=0.0,
                    )
                )
                t = datetime.fromtimestamp(kickoff + elapsed)

            times.append(t)

            recv = to_float(
                pick_value(
                    row,
                    index_map,
                    names=("unique_chunks_received", "files_received"),
                    indices=(7, 3),
                    default=0.0,
                )
            )
            recv_stat.append(recv)

            lost = to_float(
                pick_value(
                    row,
                    index_map,
                    names=("estimated_packets_lost",),
                    indices=(9,),
                    default=0.0,
                )
            )
            total = lost + recv
            loss = (lost / total * 100.0) if total > 0 else 0.0
            loss_pct.append(loss)

    return times, recv_stat, loss_pct


def main():
    sender_t, sender_packets = read_sender()
    receiver_t, receiver_stat, packet_loss_pct = read_receiver()

    fig, ax1 = plt.subplots(figsize=(10, 5))
    ax2 = ax1.twinx()

    ax1.plot(sender_t, sender_packets, label="Sender: data packets sent", marker="o")
    ax1.plot(receiver_t, receiver_stat, label="Receiver: received stat", marker="x")
    ax2.plot(receiver_t, packet_loss_pct, label="Packet loss %", linestyle="--")

    ax1.set_title("Sender/Receiver stats with packet loss over time")
    ax1.set_xlabel("Time")
    ax1.set_ylabel("Packets / Receiver stat")
    ax2.set_ylabel("Packet loss (%)")

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, loc="best")

    fig.autofmt_xdate()
    plt.tight_layout()
    output_path = "stats_plot.png"
    plt.savefig(output_path, dpi=150)
    print(f"Saved plot to {output_path}")


if __name__ == "__main__":
    main()