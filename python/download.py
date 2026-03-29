import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import socket
import ssl
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode, urlparse
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

try:
    from rich.console import Console
    from rich.progress import BarColumn, Progress, SpinnerColumn, TaskProgressColumn, TextColumn, TimeElapsedColumn
except ImportError:
    Console = None
    Progress = None
else:
    DOWNLOAD_CONSOLE = Console(force_terminal=True)


BROKER_URL = "https://broker.bgpstream.caida.org/v2/data"
ROUTEVIEWS_ARCHIVE_BASE_URL = "https://archive.routeviews.org"
CHUNK_SIZE = 1024 * 1024
DEFAULT_DATA_ROOT = "bgpdata"
DEFAULT_WORKERS = 32
REQUEST_TIMEOUT_SECONDS = 60
DEFAULT_RETRIES = 3
INITIAL_RETRY_DELAY_SECONDS = 1.0
MAX_RETRY_DELAY_SECONDS = 10.0


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
    parser.add_argument(
        "--retries",
        type=int,
        default=DEFAULT_RETRIES,
        help="Retry rounds after the initial pass for files that failed earlier",
    )
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
    with open_url(request) as response:
        return json.load(response)


def root_cause(exc: BaseException) -> BaseException:
    current = exc
    while getattr(current, "__cause__", None) is not None:
        current = current.__cause__
    return current


def explain_request_error(url: str, exc: BaseException) -> str:
    root = root_cause(exc)

    if isinstance(root, HTTPError):
        return f"HTTP {root.code} while opening {url}: {root.reason}"

    if isinstance(root, URLError):
        reason = root.reason
        reason_text = str(reason)

        if isinstance(reason, OSError) and getattr(reason, "errno", None) == 111:
            return (
                f"Connection refused while opening {url}. "
                "The target host or a configured proxy actively refused the TCP connection. "
                "Check whether your network can reach this URL directly, whether a local proxy is configured but not running, "
                "and prefer HTTPS endpoints when available."
            )

        if "CERTIFICATE_VERIFY_FAILED" in reason_text:
            return (
                f"TLS certificate verification failed for {url}. "
                "Check your CA certificates, HTTPS interception proxy, or system time."
            )

        if "timed out" in reason_text.lower():
            return f"Timed out while opening {url}. The network path is reachable but too slow or blocked."

        return f"Failed to open {url}: {reason_text}"

    if isinstance(root, ssl.SSLEOFError):
        return (
            f"TLS stream ended unexpectedly while reading {url}. "
            "This is usually a transient server or middlebox disconnect rather than corrupt data."
        )

    if isinstance(root, ssl.SSLError):
        return f"TLS error while opening {url}: {root}"

    if isinstance(root, (socket.timeout, TimeoutError)):
        return f"Timed out while opening {url}. The network path is reachable but too slow or blocked."

    if isinstance(root, OSError) and getattr(root, "errno", None) == 111:
        return (
            f"Connection refused while opening {url}. "
            "The target host or a configured proxy actively refused the TCP connection."
        )

    return f"Failed to open {url}: {root}"


def open_url(request: Request):
    try:
        return urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS)
    except (URLError, ssl.SSLError, TimeoutError, OSError) as exc:
        raise RuntimeError(explain_request_error(request.full_url, exc)) from exc


def is_retryable_error(exc: BaseException) -> bool:
    root = root_cause(exc)

    if isinstance(root, HTTPError):
        return root.code in {408, 425, 429, 500, 502, 503, 504}

    return isinstance(root, (URLError, ssl.SSLError, socket.timeout, TimeoutError, ConnectionError, OSError))


def retry_delay_seconds(attempt: int) -> float:
    return min(INITIAL_RETRY_DELAY_SECONDS * (2 ** max(attempt - 1, 0)), MAX_RETRY_DELAY_SECONDS)


def summarize_error(exc: BaseException) -> str:
    root = root_cause(exc)
    text = str(root).strip()
    return text if text else root.__class__.__name__


def download_solution_hint(destination: Path, exc: BaseException) -> str:
    root = root_cause(exc)
    partial = partial_path(destination)
    message = summarize_error(exc)

    if isinstance(root, HTTPError):
        if root.code == 404:
            return "建议: 先确认 collector、时间范围和 record_type 是否正确；这个时间片的归档文件可能本来就不存在。"
        if root.code in {429, 500, 502, 503, 504}:
            return "建议: 上游暂时不可用或限流，稍后重试，或降低 --workers。"
        return "建议: 检查远端归档服务是否可访问，以及请求参数是否正确。"

    if isinstance(root, (ssl.SSLError, URLError, socket.timeout, TimeoutError, ConnectionError)):
        return "建议: 检查网络、代理和防火墙，必要时降低 --workers 后重试。"

    if isinstance(root, OSError) and getattr(root, "errno", None) == 28:
        return "建议: 本地磁盘空间不足，先清理磁盘后再重试。"

    if "Local partial file is larger than remote file" in message or "Downloaded size mismatch" in message:
        return f"建议: 脚本会自动删除残留分片文件 {partial}，下次会从头重新下载该文件。"

    return f"建议: 检查网络、磁盘空间和文件权限；如果 {partial.name} 是残留坏分片，删除后再重试。"


def format_download_failure(resource: Resource, destination: Path, exc: BaseException) -> str:
    return (
        f"下载失败: {destination.name} | {summarize_error(exc)} | "
        f"{download_solution_hint(destination, exc)}"
    )


def probe_remote_size(url: str) -> int:
    last_error: BaseException | None = None

    for attempt in range(1, DEFAULT_RETRIES + 1):
        headers = {"Accept-Encoding": "identity"}

        try:
            request = Request(url, headers=headers, method="HEAD")
            with open_url(request) as response:
                content_length = response.headers.get("Content-Length")
                if content_length is not None:
                    return int(content_length)
        except Exception as exc:
            last_error = exc

        try:
            request = Request(url, headers={**headers, "Range": "bytes=0-0"})
            with open_url(request) as response:
                content_range = response.headers.get("Content-Range")
                content_length = response.headers.get("Content-Length")

            if content_range and "/" in content_range:
                return int(content_range.rsplit("/", 1)[1])
            if content_length is not None:
                return int(content_length)
        except Exception as exc:
            last_error = exc
            if attempt >= DEFAULT_RETRIES or not is_retryable_error(exc):
                break
            time.sleep(retry_delay_seconds(attempt))

    if last_error is not None:
        raise RuntimeError(explain_request_error(url, last_error)) from last_error
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
            f"{ROUTEVIEWS_ARCHIVE_BASE_URL}/{collector}/bgpdata/{dt:%Y.%m}/UPDATES/"
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


def partial_path(destination: Path) -> Path:
    return destination.with_name(f"{destination.name}.part")


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


def available_local_path(base_dir: Path, resource: Resource) -> Path | None:
    cache_path = cache_artifact_path(base_dir, resource)
    if cache_path.exists():
        return cache_path

    destination = destination_path(base_dir, resource)
    if destination.exists():
        return destination

    return None


def local_size(path: Path) -> int:
    if not path.exists():
        return 0
    return path.stat().st_size


def locally_available_bytes(base_dir: Path, destination: Path, resource: Resource) -> int:
    cache_path = cache_artifact_path(base_dir, resource)
    if cache_path.exists():
        return resource.remote_size if resource.remote_size > 0 else local_size(cache_path)

    if destination.exists():
        return resource.remote_size if resource.remote_size > 0 else local_size(destination)

    partial = partial_path(destination)
    if partial.exists():
        size = local_size(partial)
        if resource.remote_size > 0:
            return min(size, resource.remote_size)
        return size

    return 0


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
        if destination.exists():
            completed.add(key)
            continue
        if resource.remote_size > 0 and current[key] == resource.remote_size:
            completed.add(key)

    return ProgressTracker(totals=totals, current=current, completed=completed)


def build_download_progress(total_files: int):
    if Progress is None:
        return None, None

    progress = Progress(
        SpinnerColumn(),
        TextColumn("[bold green]download[/bold green]"),
        BarColumn(bar_width=None),
        TaskProgressColumn(),
        TextColumn("{task.completed}/{task.total} files"),
        TextColumn("{task.fields[downloaded_text]}"),
        TimeElapsedColumn(),
        console=DOWNLOAD_CONSOLE,
        refresh_per_second=4,
        expand=True,
        transient=False,
    )
    task_id = progress.add_task("download", total=total_files, downloaded_text="0.0 B")
    return progress, task_id


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


def tracker_mark_paused(tracker: ProgressTracker, key: str, current_bytes: int) -> None:
    with tracker.lock:
        tracker.current[key] = current_bytes
        tracker.active.discard(key)


def refresh_download_progress(progress, task_id: int, tracker: ProgressTracker) -> None:
    if progress is None:
        return

    downloaded_bytes, total_bytes, known_totals, completed_files, _, total_files = tracker_snapshot(tracker)
    if known_totals == total_files and total_bytes > 0:
        downloaded_text = f"{format_bytes(downloaded_bytes)}/{format_bytes(total_bytes)}"
    else:
        downloaded_text = format_bytes(downloaded_bytes)
    progress.update(
        task_id,
        completed=min(completed_files, total_files),
        downloaded_text=downloaded_text,
    )


def emit_download_log(message: str, progress=None) -> None:
    if progress is not None:
        progress.console.print(message)
    else:
        print(message, flush=True)


def should_delete_partial_on_error(exc: BaseException) -> bool:
    message = summarize_error(exc)
    return (
        "Local partial file is larger than remote file" in message
        or "Downloaded size mismatch" in message
    )


def cleanup_partial_file(destination: Path) -> int:
    partial = partial_path(destination)
    if not partial.exists():
        return 0
    size = local_size(partial)
    partial.unlink(missing_ok=True)
    return size


def cleanup_incomplete_downloads(
    pending: list[tuple[Resource, Path]],
    *,
    base_dir: Path,
    progress=None,
) -> None:
    removed_files = 0
    removed_bytes = 0

    for resource, destination in pending:
        if available_local_path(base_dir, resource) is not None:
            continue

        partial = partial_path(destination)
        if not partial.exists():
            continue

        removed_bytes += local_size(partial)
        partial.unlink(missing_ok=True)
        removed_files += 1

    if removed_files > 0:
        emit_download_log(
            f"已清理 {removed_files} 个未完成下载分片，共 {format_bytes(removed_bytes)}。",
            progress,
        )


def print_overall_progress_plain(tracker: ProgressTracker, stop_event: threading.Event) -> None:
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


def drive_download_progress(tracker: ProgressTracker, stop_event: threading.Event, progress=None, task_id: int | None = None) -> None:
    if progress is None or task_id is None:
        print_overall_progress_plain(tracker, stop_event)
        return

    refresh_download_progress(progress, task_id, tracker)
    while True:
        if stop_event.wait(0.2):
            refresh_download_progress(progress, task_id, tracker)
            return
        refresh_download_progress(progress, task_id, tracker)


def download_resource(
    resource: Resource,
    destination: Path,
    tracker: ProgressTracker,
    base_dir: Path,
    interrupt_event: threading.Event,
) -> None:
    key = str(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = partial_path(destination)

    try:
        if interrupt_event.is_set():
            raise KeyboardInterrupt("Download interrupted by user")

        cache_path = cache_artifact_path(base_dir, resource)

        if cache_path.exists():
            tracker_mark_completed(tracker, key)
            return

        # Finalized archive files are immutable; if they already exist locally, treat them as a cache hit.
        if destination.exists():
            tracker_mark_completed(tracker, key)
            return

        existing = local_size(partial)

        if resource.remote_size > 0 and existing > resource.remote_size:
            raise RuntimeError(f"Local partial file is larger than remote file: {partial}")

        if resource.remote_size == 0 and existing > 0:
            resource.remote_size = probe_remote_size(resource.url)
            tracker_set_total(tracker, key, resource.remote_size)

        if resource.remote_size > 0 and existing == resource.remote_size:
            partial.replace(destination)
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
        with open_url(request) as response:
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
            with partial.open(mode) as output:
                while True:
                    if interrupt_event.is_set():
                        raise KeyboardInterrupt("Download interrupted by user")
                    chunk = response.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    output.write(chunk)
                    current_bytes += len(chunk)
                    tracker_update_bytes(tracker, key, current_bytes)

        final_size = local_size(partial)
        if resource.remote_size > 0 and final_size != resource.remote_size:
            raise RuntimeError(
                f"Downloaded size mismatch for {partial}: got {final_size}, expected {resource.remote_size}"
            )

        partial.replace(destination)
        tracker_mark_completed(tracker, key)
    except BaseException as exc:
        if should_delete_partial_on_error(exc):
            cleanup_partial_file(destination)
            tracker_mark_paused(tracker, key, 0)
        else:
            tracker_mark_paused(tracker, key, local_size(partial))
        raise


def run_download_round(
    pending: list[tuple[Resource, Path]],
    *,
    workers: int,
    tracker: ProgressTracker,
    base_dir: Path,
    interrupt_event: threading.Event,
    progress=None,
) -> list[tuple[Resource, Path, BaseException]]:
    failures: list[tuple[Resource, Path, BaseException]] = []

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {
            executor.submit(download_resource, resource, destination, tracker, base_dir, interrupt_event): (resource, destination)
            for resource, destination in pending
        }
        try:
            for future in as_completed(futures):
                resource, destination = futures[future]
                try:
                    future.result()
                except Exception as exc:
                    failures.append((resource, destination, exc))
                    emit_download_log(format_download_failure(resource, destination, exc), progress)
        except KeyboardInterrupt:
            interrupt_event.set()
            for future in futures:
                future.cancel()
            raise

    return failures


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
    retries: int = DEFAULT_RETRIES,
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
    print(f"retry_rounds: {retries}")
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
    progress, task_id = build_download_progress(len(resources))
    stop_event = threading.Event()
    interrupt_event = threading.Event()
    progress_thread = threading.Thread(
        target=drive_download_progress,
        args=(tracker, stop_event, progress, task_id),
        daemon=True,
    )
    if progress is not None:
        progress.start()
    progress_thread.start()
    pending: list[tuple[Resource, Path]] = []
    interrupted = False

    try:
        pending = [
            (resource, destination)
            for resource, destination in zip(resources, destinations)
            if not cache_artifact_path(base_dir, resource).exists() and not destination.exists()
        ]

        if not pending:
            emit_download_log("all matched files already cached locally", progress)
            return [path for resource in resources if (path := available_local_path(base_dir, resource)) is not None]

        failures = run_download_round(
            pending,
            workers=workers,
            tracker=tracker,
            base_dir=base_dir,
            interrupt_event=interrupt_event,
            progress=progress,
        )

        for retry_round in range(1, retries + 1):
            if not failures:
                break

            delay = retry_delay_seconds(retry_round)
            emit_download_log(
                f"补下载轮次 {retry_round}/{retries}: 共有 {len(failures)} 个失败文件，等待 {delay:.1f}s 后重试。",
                progress,
            )
            time.sleep(delay)

            failures = run_download_round(
                [(resource, destination) for resource, destination, _ in failures],
                workers=workers,
                tracker=tracker,
                base_dir=base_dir,
                interrupt_event=interrupt_event,
                progress=progress,
            )

        if failures:
            emit_download_log(f"最终跳过 {len(failures)} 个下载失败文件:", progress)
            for resource, destination, exc in failures:
                cleaned_bytes = cleanup_partial_file(destination)
                if cleaned_bytes > 0:
                    emit_download_log(
                        f"已删除失败文件的残留分片: {destination.name}.part ({format_bytes(cleaned_bytes)})",
                        progress,
                    )
                emit_download_log(format_download_failure(resource, destination, exc), progress)
    except KeyboardInterrupt:
        interrupted = True
        interrupt_event.set()
        emit_download_log("收到 Ctrl+C，正在停止下载并清理未完成分片...", progress)
        raise
    finally:
        stop_event.set()
        progress_thread.join()
        if interrupted and pending:
            cleanup_incomplete_downloads(pending, base_dir=base_dir, progress=progress)
        if progress is not None:
            progress.stop()

    print("download complete")
    return [path for resource in resources if (path := available_local_path(base_dir, resource)) is not None]


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
        retries=args.retries,
        output_dir=args.output_dir,
        limit=args.limit,
        dry_run=args.dry_run,
    )


if __name__ == "__main__":
    main()
