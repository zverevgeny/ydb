# -*- coding: utf-8 -*-
"""
Stress test for LEFT JOIN with LIKE predicate.

For every requested input size the workload:
  - fills `events` (random names) and `lookup` (LIKE masks) tables,
  - runs the JOIN query TWICE:
      1. without the `ydb.EnableLikeJoinHyperscan` pragma  -> standard
         cross-join + per-row filter (the double loop),
      2. with    the `ydb.EnableLikeJoinHyperscan` pragma  -> Hyperscan
         automaton path,
  - verifies that every returned row satisfies `name LIKE mask`,
  - compares the two result sets: they MUST be identical, otherwise the
    workload fails (exit code 1),
  - prints a side-by-side comparison of the two runs (row counts and timings).

Usage:
    ./ya run ydb/tests/stress/join_like_workload --help
"""

import argparse
import fnmatch
import random
import string
import sys
import time

import ydb


# ── helpers ──────────────────────────────────────────────────────────────────


def random_name(length=8):
    """Generate a random lowercase string."""
    return "".join(random.choices(string.ascii_lowercase, k=length))


def random_mask(names):
    """
    Build a LIKE mask that is guaranteed to match at least one name.

    Strategies (randomly chosen):
      - prefix:  first N chars + "%"
      - suffix:  "%" + last N chars
      - contain: "%" + middle substring + "%"
      - exact:   the full name
    """
    name = random.choice(names)
    strategy = random.choice(["prefix", "suffix", "contain", "exact"])

    if strategy == "exact":
        return name
    if strategy == "prefix":
        n = random.randint(2, max(2, len(name) - 1))
        return name[:n] + "%"
    if strategy == "suffix":
        n = random.randint(2, max(2, len(name) - 1))
        return "%" + name[-n:]
    # contain
    if len(name) >= 4:
        start = random.randint(1, len(name) - 3)
        sub = name[start : start + random.randint(1, len(name) - start - 1)]
    else:
        sub = name[1:] if len(name) > 1 else name[0]
    return "%" + sub + "%"


def like_to_fnmatch(pattern):
    """Convert SQL LIKE pattern to fnmatch-compatible pattern.

    SQL LIKE:  %  -> any sequence,  _  -> single char
    fnmatch:   *  -> any sequence,   ?  -> single char
    """
    pattern = pattern.replace("*", "\\*").replace("?", "\\?")
    pattern = pattern.replace("%", "*").replace("_", "?")
    return pattern


def name_matches_mask(name, mask):
    """Check if `name` matches SQL LIKE `mask`."""
    return fnmatch.fnmatch(name, like_to_fnmatch(mask))


def _decode(value):
    """Decode bytes to str, leaving other values (incl. None) untouched."""
    if isinstance(value, bytes):
        return value.decode("utf-8")
    return value


# ── query helpers ──────────────────────────────────────────────────────────────


def build_query(events_table, lookup_table, use_hyperscan):
    """Build the JOIN query, optionally enabling the Hyperscan LIKE-join pragma."""
    pragma = ""
    if use_hyperscan:
        pragma = "PRAGMA ydb.EnableLikeJoinHyperscan = 'true';\n"
    return f"""
        PRAGMA ydb.HashJoinMode = 'map';
        {pragma}SELECT e.id, e.name, l.mask, l.id as mask_id
        FROM `{events_table}` AS e
        LEFT JOIN `{lookup_table}` AS l
          ON e.name LIKE l.mask
        ORDER BY e.id, mask_id
    """


def normalize_rows(rows):
    """Turn the driver rows into a list of comparable, order-stable tuples."""
    normalized = []
    for row in rows:
        normalized.append(
            (
                row["e.id"],
                _decode(row["e.name"]),
                _decode(row["l.mask"]),
                row["mask_id"],
            )
        )
    # The query is ordered by (e.id, mask_id), but make the comparison robust
    # against any residual ordering differences between the two plans.
    normalized.sort(key=lambda r: (r[0], (r[3] is None, r[3])))
    return normalized


def run_variant(pool, events_table, lookup_table, use_hyperscan):
    """Execute one variant of the query and return (rows, elapsed_seconds)."""
    query = build_query(events_table, lookup_table, use_hyperscan)
    t0 = time.monotonic()
    result = pool.execute_with_retries(query)
    elapsed = time.monotonic() - t0
    rows = normalize_rows(list(result[0].rows))
    return rows, elapsed


def verify_like(rows):
    """Verify every non-NULL row satisfies its LIKE mask. Returns (matched, null, errors)."""
    errors = 0
    matched_rows = 0
    null_rows = 0
    for _e_id, e_name, l_mask, _mask_id in rows:
        if l_mask is None:
            null_rows += 1
        else:
            matched_rows += 1
            if not name_matches_mask(e_name, l_mask):
                errors += 1
                if errors <= 10:
                    print(f"  [ERROR] name={e_name!r} does NOT match mask={l_mask!r}")
    return matched_rows, null_rows, errors


# ── table management ───────────────────────────────────────────────────────────


def create_tables(pool, events_table, lookup_table):
    pool.execute_with_retries(f"DROP TABLE IF EXISTS `{events_table}`")
    pool.execute_with_retries(f"DROP TABLE IF EXISTS `{lookup_table}`")
    pool.execute_with_retries(
        f"""
        CREATE TABLE `{events_table}` (
            id    Int64   NOT NULL,
            name  String  NOT NULL,
            PRIMARY KEY (id)
        )
        """
    )
    pool.execute_with_retries(
        f"""
        CREATE TABLE `{lookup_table}` (
            mask String  NOT NULL,
            id   Int64   NOT NULL,
            PRIMARY KEY (id)
        )
        """
    )


def fill_tables(driver, events_table_full, lookup_table_full,
                events_count, lookup_count, batch_size):
    """Populate the events/lookup tables and return the generated event names."""
    events_columns = ydb.BulkUpsertColumns()
    events_columns.add_column("id", ydb.PrimitiveType.Int64)
    events_columns.add_column("name", ydb.PrimitiveType.String)

    lookup_columns = ydb.BulkUpsertColumns()
    lookup_columns.add_column("mask", ydb.PrimitiveType.String)
    lookup_columns.add_column("id", ydb.PrimitiveType.Int64)

    event_names = [random_name() for _ in range(events_count)]
    for batch_start in range(0, events_count, batch_size):
        batch_end = min(batch_start + batch_size, events_count)
        batch_names = event_names[batch_start:batch_end]
        batch_ids = list(range(batch_start, batch_end))
        batch = [
            {"id": i, "name": name.encode("utf-8")}
            for i, name in zip(batch_ids, batch_names)
        ]
        driver.table_client.bulk_upsert(events_table_full, batch, events_columns)

    masks = [random_mask(event_names) for _ in range(lookup_count)]
    for batch_start in range(0, lookup_count, batch_size):
        batch_end = min(batch_start + batch_size, lookup_count)
        batch_masks = masks[batch_start:batch_end]
        batch = [
            {"mask": mask.encode("utf-8"), "id": i + 10}
            for i, mask in enumerate(batch_masks, start=batch_start)
        ]
        driver.table_client.bulk_upsert(lookup_table_full, batch, lookup_columns)

    return event_names


# ── per-size run ───────────────────────────────────────────────────────────────


def run_for_size(driver, pool, database, path, events_count, lookup_count, batch_size):
    """
    Run both variants (standard vs Hyperscan) for a single input size.

    Returns a dict with the collected metrics for the final comparison table,
    and raises AssertionError-style failures by returning ok=False.
    """
    size_path = f"{path}/e{events_count}_l{lookup_count}"
    events_table = f"{size_path}/events"
    lookup_table = f"{size_path}/lookup"
    events_table_full = f"{database}/{events_table}"
    lookup_table_full = f"{database}/{lookup_table}"

    print()
    print("#" * 70)
    print(f"# Input size: events={events_count:,}  lookup masks={lookup_count:,}")
    print("#" * 70)

    print("Creating tables...")
    create_tables(pool, events_table, lookup_table)

    print(f"Filling tables (events={events_count:,}, masks={lookup_count:,})...")
    t0 = time.monotonic()
    fill_tables(driver, events_table_full, lookup_table_full,
                events_count, lookup_count, batch_size)
    print(f"  Filled in {time.monotonic() - t0:.1f}s")

    # ── standard (no pragma) ──────────────────────────────────────────────
    print("Running WITHOUT pragma (standard cross-join + filter)...")
    std_rows, std_elapsed = run_variant(pool, events_table, lookup_table, use_hyperscan=False)
    std_matched, std_null, std_errors = verify_like(std_rows)
    print(f"  rows={len(std_rows):,}  matched={std_matched:,}  null={std_null:,}  time={std_elapsed:.2f}s")

    # ── Hyperscan (with pragma) ───────────────────────────────────────────
    print("Running WITH pragma (Hyperscan automaton)...")
    hs_rows, hs_elapsed = run_variant(pool, events_table, lookup_table, use_hyperscan=True)
    hs_matched, hs_null, hs_errors = verify_like(hs_rows)
    print(f"  rows={len(hs_rows):,}  matched={hs_matched:,}  null={hs_null:,}  time={hs_elapsed:.2f}s")

    # ── compare the two result sets ───────────────────────────────────────
    identical = std_rows == hs_rows
    if not identical:
        print("  [MISMATCH] result sets differ between pragma on/off!")
        # Report the first few differing rows to aid debugging.
        std_set = set(std_rows)
        hs_set = set(hs_rows)
        only_std = [r for r in std_rows if r not in hs_set]
        only_hs = [r for r in hs_rows if r not in std_set]
        for r in only_std[:10]:
            print(f"    only WITHOUT pragma: {r!r}")
        for r in only_hs[:10]:
            print(f"    only WITH    pragma: {r!r}")

    ok = identical and std_errors == 0 and hs_errors == 0

    return {
        "events": events_count,
        "lookup": lookup_count,
        "std_rows": len(std_rows),
        "hs_rows": len(hs_rows),
        "std_time": std_elapsed,
        "hs_time": hs_elapsed,
        "std_errors": std_errors,
        "hs_errors": hs_errors,
        "identical": identical,
        "ok": ok,
    }


# ── main ─────────────────────────────────────────────────────────────────────


def parse_sizes(raw, default):
    """Parse --sizes 'E:L,E:L,...' into a list of (events, lookup) pairs."""
    if not raw:
        return default
    sizes = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        events_str, _, lookup_str = item.partition(":")
        sizes.append((int(events_str), int(lookup_str)))
    return sizes


def main():
    parser = argparse.ArgumentParser(
        description="Stress test for LEFT JOIN with LIKE predicate (pragma on/off comparison)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--endpoint", default="localhost:2135", help="YDB endpoint")
    parser.add_argument("--database", default="/Root", help="YDB database path")
    parser.add_argument("--path", default="join_like_stress", help="Table path prefix")
    parser.add_argument(
        "--sizes",
        default="",
        help="Comma-separated list of input sizes as EVENTS:LOOKUP "
             "(e.g. '100:10,1000:50,10000:100'). Overrides the default set.",
    )
    parser.add_argument("--batch-size", default=1_000, type=int, help="Bulk upsert batch size")
    args = parser.parse_args()

    # Default set of increasing input sizes.
    default_sizes = [(100, 10), (1_000, 50), (10_000, 100)]
    sizes = parse_sizes(args.sizes, default_sizes)

    # Ensure endpoint has scheme
    endpoint = args.endpoint
    if not endpoint.startswith("grpc://") and not endpoint.startswith("grpcs://"):
        endpoint = f"grpc://{endpoint}"
    # Ensure database starts with /
    database = args.database
    if not database.startswith("/"):
        database = f"/{database}"

    print(f"Connecting to {endpoint}{database} ...")
    driver = ydb.Driver(endpoint=endpoint, database=database, oauth=None)
    driver.wait(timeout=10, fail_fast=True)
    pool = ydb.QuerySessionPool(driver, size=1)

    results = []
    try:
        for events_count, lookup_count in sizes:
            results.append(
                run_for_size(
                    driver, pool, database, args.path,
                    events_count, lookup_count, args.batch_size,
                )
            )
    finally:
        pool.stop()
        driver.stop()

    # ── final comparison table ────────────────────────────────────────────
    print()
    print("=" * 94)
    print("Comparison of runs WITHOUT pragma (standard) vs WITH pragma (Hyperscan)")
    print("=" * 94)
    header = (
        f"{'events':>10} {'masks':>8} | "
        f"{'rows(off)':>10} {'rows(on)':>10} | "
        f"{'time(off)':>10} {'time(on)':>10} {'speedup':>8} | "
        f"{'identical':>10}"
    )
    print(header)
    print("-" * len(header))
    for r in results:
        speedup = (r["std_time"] / r["hs_time"]) if r["hs_time"] > 0 else float("inf")
        print(
            f"{r['events']:>10,} {r['lookup']:>8,} | "
            f"{r['std_rows']:>10,} {r['hs_rows']:>10,} | "
            f"{r['std_time']:>9.2f}s {r['hs_time']:>9.2f}s {speedup:>7.2f}x | "
            f"{('YES' if r['identical'] else 'NO'):>10}"
        )
    print("=" * 94)

    all_ok = all(r["ok"] for r in results)
    if not all_ok:
        for r in results:
            if not r["ok"]:
                reason = []
                if not r["identical"]:
                    reason.append("results differ between pragma on/off")
                if r["std_errors"]:
                    reason.append(f"{r['std_errors']} LIKE errors without pragma")
                if r["hs_errors"]:
                    reason.append(f"{r['hs_errors']} LIKE errors with pragma")
                print(
                    f"FAIL: size events={r['events']:,} masks={r['lookup']:,}: "
                    + "; ".join(reason)
                )
        sys.exit(1)

    print("PASS: pragma on/off produce identical, correctly verified results for all sizes.")


if __name__ == "__main__":
    main()
