import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode, urlparse
from urllib.request import Request, urlopen


BROKER_URL = "https://broker.bgpstream.caida.org/v2/data"
CHUNK_SIZE = 1024 * 1024
DEFAULT_DATA_ROOT = "bgpdata"
DEFAULT_WORKERS = 32


@dataclass
class Resource:
    url: str
    project: str
    collector: str
    record_type: str
    initial_time: int
    duration: int
    remote_size: int


@dataclass
class ProgressTracker:
    totals: dict[str, int]
    current: dict[str, int]
    active: set[str] = field(default_factory=set)
    completed: set[str] = field(default_factory=set)
    lock: threading.Lock = field(default_factory=threading.Lock)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download BGPStream MRT files for a time range with progress and resume support."
    )
    parser.add_argument("--from-time", required=True, help="Start time, e.g. 2017-07-07 or 2017-07-07 00:00:00 UTC")
    parser.add_argument("--until-time", required=True, help="End time, e.g. 2017-07-07 01:00:00 UTC")
    parser.add_argument("--collector", default="route-views.sg", help="Collector name, e.g. route-views.sg")
    parser.add_argument("--project", default=None, help="Optional project filter, e.g. routeviews or ris")
    parser.add_argument("--record-type", choices=["updates", "ribs"], default="updates")
    parser.add_argument(
        "--source",
        choices=["auto", "broker", "routeviews-direct"],
        default="auto",
        help="How to resolve remote files. 'auto' prefers direct Route Views URLs for route-views.* updates.",
    )
    parser.add_argument(
        "--probe-size",
        action="store_true",
        help="Probe every remote file size before downloading. Slower startup, but total size is known upfront.",
    )
    parser.add_argument("--workers", type=int, default=DEFAULT_WORKERS, help="Number of files to download in parallel")
    parser.add_argument("--output-dir", default=DEFAULT_DATA_ROOT, help="Shared data root where files will be stored")
    parser.add_argument("--limit", type=int, default=None, help="Only download the first N matched resources")
    parser.add_argument("--dry-run", action="store_true", help="Only list matched resources, do not download")
    return parser.parse_args()


def parse_time(value: str) -> int:
    value = value.strip()

    if value.endswith(" UTC"):
        value = value[:-4]

    for fmt in ("%Y-%m-%d", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            dt = datetime.strptime(value, fmt)
            return int(dt.replace(tzinfo=timezone.utc).timestamp())
        except ValueError:
            pass

    try:
        dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise SystemExit(f"Unsupported time format: {value}") from exc

    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def format_bytes(num_bytes: int) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    value = float(num_bytes)
    for unit in units:
        if value < 1024 or unit == units[-1]:
            return f"{value:.1f} {unit}"
        value /= 1024
    return f"{num_bytes} B"


def fetch_json(url: str) -> dict:
    request = Request(url, headers={"Accept-Encoding": "identity"})
    with urlopen(request) as response:
        return json.load(response)


def probe_remote_size(url: str) -> int:
    headers = {"Accept-Encoding": "identity"}

    try:
        request = Request(url, headers=headers, method="HEAD")
        with urlopen(request) as response:
            content_length = response.headers.get("Content-Length")
            if content_length is not None:
                return int(content_length)
    except Exception:
        pass

    request = Request(url, headers={**headers, "Range": "bytes=0-0"})
    with urlopen(request) as response:
        content_range = response.headers.get("Content-Range")
        content_length = response.headers.get("Content-Length")

    if content_range and "/" in content_range:
        return int(content_range.rsplit("/", 1)[1])
    if content_length is not None:
        return int(content_length)
    raise RuntimeError(f"Could not determine remote size for {url}")


def fetch_resources_via_broker(
    start_epoch: int,
    end_epoch: int,
    collector: str,
    project: str | None,
    record_type: str,
) -> list[Resource]:
    params = [
        ("collectors[]", collector),
        ("types[]", record_type),
        ("intervals[]", f"{start_epoch},{end_epoch}"),
    ]
    if project:
        params.append(("projects[]", project))

    payload = fetch_json(f"{BROKER_URL}?{urlencode(params)}")
    resources = []

    for item in payload["data"]["resources"]:
        resources.append(
            Resource(
                url=item["url"],
                project=item["project"],
                collector=item["collector"],
                record_type=item["type"],
                initial_time=item["initialTime"],
                duration=item["duration"],
                remote_size=0,
            )
        )

    resources.sort(key=lambda item: (item.initial_time, item.collector, item.url))
    return resources


def fetch_resources_via_routeviews_direct(start_epoch: int, end_epoch: int, collector: str, record_type: str) -> list[Resource]:
    if record_type != "updates":
        raise SystemExit("routeviews-direct currently supports updates only")

    slot = (start_epoch // 900) * 900 - 900
    last_slot = (end_epoch // 900) * 900
    resources = []

    while slot <= last_slot:
        dt = datetime.fromtimestamp(slot, tz=timezone.utc)
        url = (
            f"http://archive.routeviews.org/{collector}/bgpdata/{dt:%Y.%m}/UPDATES/"
            f"updates.{dt:%Y%m%d.%H%M}.bz2"
        )
        resources.append(
            Resource(
                url=url,
                project="routeviews",
                collector=collector,
                record_type="updates",
                initial_time=slot,
                duration=900,
                remote_size=0,
            )
        )
        slot += 900

    return resources


def resolve_resources(
    start_epoch: int,
    end_epoch: int,
    collector: str,
    project: str | None,
    record_type: str,
    source: str,
) -> tuple[str, list[Resource]]:
    if source == "broker":
        return (
            "broker",
            fetch_resources_via_broker(
                start_epoch=start_epoch,
                end_epoch=end_epoch,
                collector=collector,
                project=project,
                record_type=record_type,
            ),
        )

    if source == "routeviews-direct":
        return (
            "routeviews-direct",
            fetch_resources_via_routeviews_direct(
                start_epoch=start_epoch,
                end_epoch=end_epoch,
                collector=collector,
                record_type=record_type,
            ),
        )

    if collector.startswith("route-views") and record_type == "updates":
        return (
            "routeviews-direct",
            fetch_resources_via_routeviews_direct(
                start_epoch=start_epoch,
                end_epoch=end_epoch,
                collector=collector,
                record_type=record_type,
            ),
        )

    return (
        "broker",
        fetch_resources_via_broker(
            start_epoch=start_epoch,
            end_epoch=end_epoch,
            collector=collector,
            project=project,
            record_type=record_type,
        ),
    )


def attach_remote_sizes(resources: list[Resource]) -> None:
    total = len(resources)
    for index, resource in enumerate(resources, start=1):
        print(f"probing size {index}/{total}: {Path(urlparse(resource.url).path).name}", flush=True)
        resource.remote_size = probe_remote_size(resource.url)


def destination_path(base_dir: Path, resource: Resource) -> Path:
    filename = Path(urlparse(resource.url).path).name
    return base_dir / resource.project / resource.collector / resource.record_type / filename


def cache_artifact_path(base_dir: Path, resource: Resource) -> Path:
    return (
        base_dir
        / resource.project
        / resource.collector
        / resource.record_type
        / f"{resource.project}.{resource.collector}.{resource.record_type}.{resource.initial_time}.{resource.duration}.cache"
    )


def preferred_local_path(base_dir: Path, resource: Resource) -> Path:
    cache_path = cache_artifact_path(base_dir, resource)
    if cache_path.exists():
        return cache_path
    return destination_path(base_dir, resource)


def local_size(path: Path) -> int:
    if not path.exists():
        return 0
    return path.stat().st_size


def locally_available_bytes(base_dir: Path, destination: Path, resource: Resource) -> int:
    cache_path = cache_artifact_path(base_dir, resource)
    if cache_path.exists():
        return resource.remote_size if resource.remote_size > 0 else local_size(cache_path)

    size = local_size(destination)
    if resource.remote_size > 0:
        return min(size, resource.remote_size)
    return size


def build_progress_tracker(resources: list[Resource], destinations: list[Path], base_dir: Path) -> ProgressTracker:
    totals = {}
    current = {}
    completed = set()

    for resource, destination in zip(resources, destinations):
        key = str(destination)
        totals[key] = resource.remote_size
        current[key] = locally_available_bytes(base_dir, destination, resource)
        if cache_artifact_path(base_dir, resource).exists():
            completed.add(key)
            continue
        if resource.remote_size > 0 and current[key] == resource.remote_size:
            completed.add(key)

    return ProgressTracker(totals=totals, current=current, completed=completed)


def tracker_snapshot(tracker: ProgressTracker) -> tuple[int, int, int, int, int, int]:
    with tracker.lock:
        downloaded_bytes = sum(tracker.current.values())
        total_bytes = sum(total for total in tracker.totals.values() if total > 0)
        known_totals = sum(1 for total in tracker.totals.values() if total > 0)
        completed_files = len(tracker.completed)
        active_files = len(tracker.active)
        total_files = len(tracker.totals)
    return downloaded_bytes, total_bytes, known_totals, completed_files, active_files, total_files


def tracker_mark_started(tracker: ProgressTracker, key: str, current_bytes: int) -> None:
    with tracker.lock:
        tracker.current[key] = current_bytes
        if tracker.totals[key] == 0 or current_bytes < tracker.totals[key]:
            tracker.active.add(key)


def tracker_update_bytes(tracker: ProgressTracker, key: str, current_bytes: int) -> None:
    with tracker.lock:
        tracker.current[key] = current_bytes


def tracker_set_total(tracker: ProgressTracker, key: str, total_bytes: int) -> None:
    with tracker.lock:
        tracker.totals[key] = total_bytes
        if tracker.current[key] == total_bytes:
            tracker.active.discard(key)
            tracker.completed.add(key)


def tracker_mark_completed(tracker: ProgressTracker, key: str) -> None:
    with tracker.lock:
        if tracker.totals[key] > 0:
            tracker.current[key] = tracker.totals[key]
        tracker.active.discard(key)
        tracker.completed.add(key)


def print_overall_progress(tracker: ProgressTracker, stop_event: threading.Event) -> None:
    downloaded_bytes, _, _, _, _, _ = tracker_snapshot(tracker)
    last_bytes = downloaded_bytes
    last_time = time.time()

    while True:
        downloaded_bytes, total_bytes, known_totals, completed_files, active_files, total_files = tracker_snapshot(tracker)
        now = time.time()
        speed = max(downloaded_bytes - last_bytes, 0) / max(now - last_time, 1e-6)
        if known_totals == total_files and total_bytes > 0:
            pct = downloaded_bytes / total_bytes * 100.0
            line = (
                "\r"
                f"files={completed_files}/{total_files} "
                f"active={active_files} "
                f"downloaded={format_bytes(downloaded_bytes)}/{format_bytes(total_bytes)} "
                f"({pct:5.1f}%) "
                f"speed={format_bytes(int(speed))}/s"
            )
        else:
            line = (
                "\r"
                f"files={completed_files}/{total_files} "
                f"active={active_files} "
                f"downloaded={format_bytes(downloaded_bytes)} "
                f"known_totals={known_totals}/{total_files} "
                f"speed={format_bytes(int(speed))}/s"
            )
        print(line, end="", flush=True)

        if stop_event.wait(0.2):
            downloaded_bytes, total_bytes, known_totals, completed_files, active_files, total_files = tracker_snapshot(tracker)
            if known_totals == total_files and total_bytes > 0:
                pct = downloaded_bytes / total_bytes * 100.0
                line = (
                    "\r"
                    f"files={completed_files}/{total_files} "
                    f"active={active_files} "
                    f"downloaded={format_bytes(downloaded_bytes)}/{format_bytes(total_bytes)} "
                    f"({pct:5.1f}%) "
                    "speed=0.0 B/s"
                )
            else:
                line = (
                    "\r"
                    f"files={completed_files}/{total_files} "
                    f"active={active_files} "
                    f"downloaded={format_bytes(downloaded_bytes)} "
                    f"known_totals={known_totals}/{total_files} "
                    "speed=0.0 B/s"
                )
            print(line, flush=True)
            return

        last_bytes = downloaded_bytes
        last_time = now


def download_resource(
    resource: Resource,
    destination: Path,
    tracker: ProgressTracker,
    base_dir: Path,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    existing = local_size(destination)
    key = str(destination)
    cache_path = cache_artifact_path(base_dir, resource)

    if cache_path.exists():
        tracker_mark_completed(tracker, key)
        return

    if resource.remote_size > 0 and existing > resource.remote_size:
        raise RuntimeError(f"Local file is larger than remote file: {destination}")

    if resource.remote_size == 0 and existing > 0:
        resource.remote_size = probe_remote_size(resource.url)
        tracker_set_total(tracker, key, resource.remote_size)

    if resource.remote_size > 0 and existing == resource.remote_size:
        tracker_mark_completed(tracker, key)
        return

    headers = {"Accept-Encoding": "identity"}
    mode = "wb"
    resume_from = existing

    if existing > 0:
        headers["Range"] = f"bytes={existing}-"
        mode = "ab"

    tracker_mark_started(tracker, key, existing)
    request = Request(resource.url, headers=headers)
    with urlopen(request) as response:
        status = getattr(response, "status", 200)
        content_range = response.headers.get("Content-Range")
        content_length = response.headers.get("Content-Length")

        if resource.remote_size == 0:
            if content_range and "/" in content_range:
                resource.remote_size = int(content_range.rsplit("/", 1)[1])
            elif content_length is not None:
                resource.remote_size = existing + int(content_length) if status == 206 else int(content_length)
            if resource.remote_size > 0:
                tracker_set_total(tracker, key, resource.remote_size)

        if existing > 0 and status != 206:
            resume_from = 0
            mode = "wb"
            tracker_update_bytes(tracker, key, 0)

        current_bytes = resume_from
        with destination.open(mode) as output:
            while True:
                chunk = response.read(CHUNK_SIZE)
                if not chunk:
                    break
                output.write(chunk)
                current_bytes += len(chunk)
                tracker_update_bytes(tracker, key, current_bytes)

    final_size = local_size(destination)
    if resource.remote_size > 0 and final_size != resource.remote_size:
        raise RuntimeError(
            f"Downloaded size mismatch for {destination}: got {final_size}, expected {resource.remote_size}"
        )

    tracker_mark_completed(tracker, key)


def run_download(
    *,
    from_time: str | int,
    until_time: str | int,
    collector: str = "route-views.sg",
    project: str | None = None,
    record_type: str = "updates",
    source: str = "auto",
    probe_size: bool = False,
    workers: int = DEFAULT_WORKERS,
    output_dir: str | Path = DEFAULT_DATA_ROOT,
    limit: int | None = None,
    dry_run: bool = False,
) -> list[Path]:
    base_dir = Path(output_dir)
    start_epoch = parse_time(str(from_time)) if not isinstance(from_time, int) else from_time
    end_epoch = parse_time(str(until_time)) if not isinstance(until_time, int) else until_time

    if end_epoch <= start_epoch:
        raise SystemExit("--until-time must be greater than --from-time")

    print("fetching resource list...", flush=True)
    source_name, resources = resolve_resources(
        start_epoch=start_epoch,
        end_epoch=end_epoch,
        collector=collector,
        project=project,
        record_type=record_type,
        source=source,
    )

    if limit is not None:
        resources = resources[:limit]

    if not resources:
        print("No matching resources found.")
        return []

    if probe_size:
        attach_remote_sizes(resources)

    destinations = [destination_path(base_dir, resource) for resource in resources]
    known_remote_size = sum(resource.remote_size for resource in resources if resource.remote_size > 0)
    total_local_size = 0
    for destination, resource in zip(destinations, resources):
        total_local_size += locally_available_bytes(base_dir, destination, resource)

    print(f"collector: {collector}")
    print(f"record_type: {record_type}")
    print(f"source: {source_name}")
    print(f"probe_size: {probe_size}")
    print(f"workers: {workers}")
    print(f"matched_files: {len(resources)}")
    if probe_size:
        print(f"total_remote_size: {format_bytes(known_remote_size)}")
        print(f"local_progress: {format_bytes(total_local_size)}/{format_bytes(known_remote_size)}")
    else:
        print(f"local_progress: {format_bytes(total_local_size)}")
    print(f"output_dir: {base_dir.resolve()}")

    if dry_run:
        for resource, destination in zip(resources, destinations):
            size_text = format_bytes(resource.remote_size) if resource.remote_size > 0 else "unknown"
            print(
                f"{resource.initial_time} {resource.collector} {resource.record_type} "
                f"{size_text} -> {destination}"
            )
        return [preferred_local_path(base_dir, resource) for resource in resources]

    if workers < 1:
        raise SystemExit("--workers must be at least 1")

    tracker = build_progress_tracker(resources, destinations, base_dir)
    stop_event = threading.Event()
    progress_thread = threading.Thread(
        target=print_overall_progress,
        args=(tracker, stop_event),
        daemon=True,
    )
    progress_thread.start()

    try:
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [
                executor.submit(download_resource, resource, destination, tracker, base_dir)
                for resource, destination in zip(resources, destinations)
            ]
            for future in as_completed(futures):
                future.result()
    finally:
        stop_event.set()
        progress_thread.join()

    print("download complete")
    return [preferred_local_path(base_dir, resource) for resource in resources]


def main() -> None:
    args = parse_args()
    run_download(
        from_time=args.from_time,
        until_time=args.until_time,
        collector=args.collector,
        project=args.project,
        record_type=args.record_type,
        source=args.source,
        probe_size=args.probe_size,
        workers=args.workers,
        output_dir=args.output_dir,
        limit=args.limit,
        dry_run=args.dry_run,
    )


if __name__ == "__main__":
    main()
