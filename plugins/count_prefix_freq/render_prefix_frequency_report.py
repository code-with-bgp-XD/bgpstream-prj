#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def format_pct(value: float) -> str:
    return f"{value:.2f}%"


def load_summary(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def render_svg(summary: dict) -> str:
    coverage_rows = summary.get("coverage_rows", [])
    width = 1180
    row_height = 72
    top_height = 180
    bottom_padding = 44
    header_height = 56
    table_top = top_height + 26
    height = table_top + header_height + row_height * max(1, len(coverage_rows)) + bottom_padding

    card_x = 48
    card_y = 34
    card_w = width - card_x * 2
    card_h = height - 50

    col_x = {
        "label": 82,
        "prefixes": 264,
        "messages": 420,
        "all_pct": 650,
        "bar": 840,
    }

    title = "Prefix Frequency Coverage Report"
    subtitle = (
        f"Processor: {summary.get('processor', 'unknown')} | "
        f"Generated: {summary.get('generated_at_utc', 'unknown')}"
    )
    totals = [
        ("All messages", str(summary.get("total_messages_seen", 0))),
        ("Prefixed messages", str(summary.get("messages_with_prefix", 0))),
        ("Unique prefixes", str(summary.get("unique_prefixes", 0))),
        ("Counts CSV", Path(summary.get("counts_csv", "")).name or "-"),
    ]

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<defs>",
        '<linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">',
        '<stop offset="0%" stop-color="#f7f4ed"/>',
        '<stop offset="100%" stop-color="#e7eef7"/>',
        "</linearGradient>",
        '<linearGradient id="card" x1="0%" y1="0%" x2="0%" y2="100%">',
        '<stop offset="0%" stop-color="#fffdf9"/>',
        '<stop offset="100%" stop-color="#f7fbff"/>',
        "</linearGradient>",
        '<linearGradient id="bar" x1="0%" y1="0%" x2="100%" y2="0%">',
        '<stop offset="0%" stop-color="#2463eb"/>',
        '<stop offset="100%" stop-color="#4cb7a5"/>',
        "</linearGradient>",
        '<filter id="shadow" x="-10%" y="-10%" width="120%" height="120%">',
        '<feDropShadow dx="0" dy="10" stdDeviation="14" flood-color="#17324d" flood-opacity="0.12"/>',
        "</filter>",
        "</defs>",
        f'<rect width="{width}" height="{height}" fill="url(#bg)"/>',
        f'<rect x="{card_x}" y="{card_y}" width="{card_w}" height="{card_h}" rx="28" fill="url(#card)" filter="url(#shadow)"/>',
        f'<text x="{card_x + 34}" y="88" font-size="34" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#0f2740">{title}</text>',
        f'<text x="{card_x + 34}" y="120" font-size="16" font-family="DejaVu Sans, Arial, sans-serif" fill="#47607c">{subtitle}</text>',
    ]

    stat_x = card_x + 34
    stat_y = 146
    stat_w = 250
    stat_gap = 18
    for index, (label, value) in enumerate(totals):
        x = stat_x + index * (stat_w + stat_gap)
        parts.extend(
            [
                f'<rect x="{x}" y="{stat_y}" width="{stat_w}" height="68" rx="18" fill="#edf4fb" stroke="#dbe7f3"/>',
                f'<text x="{x + 18}" y="{stat_y + 26}" font-size="14" font-family="DejaVu Sans, Arial, sans-serif" fill="#5a728c">{label}</text>',
                f'<text x="{x + 18}" y="{stat_y + 52}" font-size="24" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#12304d">{value}</text>',
            ]
        )

    header_y = table_top
    parts.extend(
        [
            f'<rect x="{card_x + 24}" y="{header_y}" width="{card_w - 48}" height="{header_height}" rx="18" fill="#12304d"/>',
            f'<text x="{col_x["label"]}" y="{header_y + 35}" font-size="15" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#f8fbff">Bucket</text>',
            f'<text x="{col_x["prefixes"]}" y="{header_y + 35}" font-size="15" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#f8fbff">Prefixes</text>',
            f'<text x="{col_x["messages"]}" y="{header_y + 35}" font-size="15" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#f8fbff">Covered messages</text>',
            f'<text x="{col_x["all_pct"]}" y="{header_y + 35}" font-size="15" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#f8fbff">Share of all</text>',
        ]
    )

    if not coverage_rows:
        coverage_rows = [
            {
                "label": "no_data",
                "prefix_count": 0,
                "covered_messages": 0,
                "share_of_all_messages_pct": 0.0,
            }
        ]

    for index, row in enumerate(coverage_rows):
        y = header_y + header_height + 10 + index * row_height
        fill = "#ffffff" if index % 2 == 0 else "#f7fbff"
        label = row["label"].replace("_", " ").upper()
        share_all = float(row["share_of_all_messages_pct"])
        bar_width = 150
        filled = max(0.0, min(bar_width, bar_width * share_all / 100.0))

        parts.extend(
            [
                f'<rect x="{card_x + 24}" y="{y}" width="{card_w - 48}" height="{row_height - 8}" rx="18" fill="{fill}" stroke="#e4edf5"/>',
                f'<text x="{col_x["label"]}" y="{y + 39}" font-size="20" font-family="DejaVu Sans, Arial, sans-serif" font-weight="700" fill="#143553">{label}</text>',
                f'<text x="{col_x["prefixes"]}" y="{y + 39}" font-size="18" font-family="DejaVu Sans, Arial, sans-serif" fill="#143553">{row["prefix_count"]}</text>',
                f'<text x="{col_x["messages"]}" y="{y + 39}" font-size="18" font-family="DejaVu Sans, Arial, sans-serif" fill="#143553">{row["covered_messages"]}</text>',
                f'<text x="{col_x["all_pct"]}" y="{y + 39}" font-size="18" font-family="DejaVu Sans, Arial, sans-serif" fill="#143553">{format_pct(share_all)}</text>',
                f'<rect x="{col_x["bar"]}" y="{y + 20}" width="{bar_width}" height="18" rx="9" fill="#dbe7f3"/>',
                f'<rect x="{col_x["bar"]}" y="{y + 20}" width="{filled}" height="18" rx="9" fill="url(#bar)"/>',
            ]
        )

    footer = "Percentages are computed from the prefix frequency distribution. Ranking uses all non-empty prefixes."
    parts.append(
        f'<text x="{card_x + 34}" y="{height - 20}" font-size="14" font-family="DejaVu Sans, Arial, sans-serif" fill="#60778f">{footer}</text>'
    )
    parts.append("</svg>")
    return "\n".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description="Render prefix frequency summary as an SVG table.")
    parser.add_argument("--input", required=True, help="Path to summary JSON")
    parser.add_argument("--output", required=True, help="Path to output SVG")
    args = parser.parse_args()

    summary = load_summary(Path(args.input))
    svg = render_svg(summary)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg, encoding="utf-8")


if __name__ == "__main__":
    main()
