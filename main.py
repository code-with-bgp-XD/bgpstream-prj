import re
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

from pybgpstream import BGPStream
from rich.console import Console
from rich.progress import BarColumn, Progress, SpinnerColumn, TaskProgressColumn, TextColumn, TimeElapsedColumn

import download


START_DATE = "2017-01-01"
END_DATE = "2018-01-01"
PROJECT = "routeviews"
COLLECTOR = "route-views.sg"
DATA_ROOT = Path(download.DEFAULT_DATA_ROOT)
DATA_DIR = DATA_ROOT / PROJECT / COLLECTOR / "updates"
DOWNLOAD_WORKERS = download.DEFAULT_WORKERS
AS_NUMBER_RE = re.compile(r"\d+")
STATS_REPORT_INTERVAL_SECONDS = 1.0
RICH_CONSOLE = Console(force_terminal=True)


def parse_closed_date_range(start_date: str, end_date: str) -> tuple[datetime, datetime]:
    start = datetime.strptime(start_date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    end_inclusive = datetime.strptime(end_date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    if end_inclusive < start:
        raise SystemExit("END_DATE must be greater than or equal to START_DATE")
    return start, end_inclusive + timedelta(days=1)


def extract_as_numbers(as_path: str) -> set[str]:
    return set(AS_NUMBER_RE.findall(as_path))


def build_stats_progress(total_files: int) -> tuple[Progress, int]:
    progress = Progress(
        SpinnerColumn(),
        TextColumn("[bold cyan]stats[/bold cyan]"),
        BarColumn(bar_width=None),
        TaskProgressColumn(),
        TextColumn("{task.completed}/{task.total} files"),
        TimeElapsedColumn(),
        console=RICH_CONSOLE,
        refresh_per_second=4,
        expand=True,
        transient=False,
    )
    task_id = progress.add_task("stats", total=total_files)
    return progress, task_id


def refresh_stats_progress(
    progress: Progress,
    task_id: int,
    processed_files: int,
) -> None:
    progress.update(task_id, completed=processed_files)


def collect_prefix_stats(files: list[Path], start: datetime, end: datetime) -> tuple[dict[str, set[str]], int, int]:
    start_ts = start.timestamp()
    end_ts = end.timestamp()
    total_files = len(files)
    last_report = 0.0

    prefix_to_ases: dict[str, set[str]] = {}
    scanned_update_count = 0
    usable_update_count = 0

    progress, task_id = build_stats_progress(total_files)
    with progress:
        for index, file_path in enumerate(files, start=1):
            stream = BGPStream(data_interface="singlefile")
            stream.set_data_interface_option("singlefile", "upd-file", str(file_path))

            for elem in stream:
                now = time.time()
                if now - last_report >= STATS_REPORT_INTERVAL_SECONDS:
                    refresh_stats_progress(
                        progress=progress,
                        task_id=task_id,
                        processed_files=index - 1,
                    )
                    last_report = now

                if elem.type != "A":
                    continue
                if not (start_ts <= elem.time < end_ts):
                    continue

                scanned_update_count += 1
                prefix = elem.fields.get("prefix")
                as_path = elem.fields.get("as-path", "")
                if not prefix or not as_path:
                    continue

                usable_update_count += 1
                prefix_to_ases.setdefault(prefix, set()).update(extract_as_numbers(as_path))

            refresh_stats_progress(
                progress=progress,
                task_id=task_id,
                processed_files=index,
            )
            last_report = time.time()

        refresh_stats_progress(
            progress=progress,
            task_id=task_id,
            processed_files=total_files,
        )
    return prefix_to_ases, scanned_update_count, usable_update_count


def main() -> None:
    start, end = parse_closed_date_range(START_DATE, END_DATE)
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    from_time = start.strftime("%Y-%m-%d %H:%M:%S UTC")
    until_time = end.strftime("%Y-%m-%d %H:%M:%S UTC")

    print("download phase", flush=True)
    files = download.run_download(
        from_time=from_time,
        until_time=until_time,
        collector=COLLECTOR,
        project=PROJECT,
        record_type="updates",
        source="auto",
        probe_size=False,
        workers=DOWNLOAD_WORKERS,
        output_dir=DATA_ROOT,
        dry_run=False,
    )

    if not files:
        raise SystemExit("No local files available for statistics.")

    print("stats phase", flush=True)
    prefix_to_ases, scanned_update_count, usable_update_count = collect_prefix_stats(files, start, end)
    total_prefix_scoped_ases = sum(len(ases) for ases in prefix_to_ases.values())

    print(f"start_date: {START_DATE}")
    print(f"end_date: {END_DATE}")
    print(f"collector: {COLLECTOR}")
    print(f"data_dir: {DATA_DIR.resolve()}")
    print(f"download_workers: {DOWNLOAD_WORKERS}")
    print(f"files_used: {len(files)}")
    print(f"scanned_update_elements: {scanned_update_count}")
    print(f"usable_update_elements: {usable_update_count}")
    print(f"unique_prefixes: {len(prefix_to_ases)}")
    print(f"prefix_scoped_as_total: {total_prefix_scoped_ases}")


if __name__ == "__main__":
    main()
