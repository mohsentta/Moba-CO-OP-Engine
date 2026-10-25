"""Grep the community hash dictionaries for path patterns.

    python scripts/hash_grep.py mosstomper,scorchclaw,gustwalker
    python scripts/hash_grep.py map11 --ext .scb,.skn --limit 40
"""
import argparse
import glob
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("patterns", help="comma-separated case-insensitive substrings")
    parser.add_argument("--ext", default="", help="comma-separated extension filter")
    parser.add_argument("--limit", type=int, default=25)
    args = parser.parse_args()

    patterns = [p.strip() for p in args.patterns.split(",") if p.strip()]
    extensions = [e.strip().lower() for e in args.ext.split(",") if e.strip()]

    matchers = [re.compile(re.escape(p), re.IGNORECASE) for p in patterns]
    hash_re = re.compile(r"^[0-9a-fA-F]{16}$")

    found = {}
    scanned = 0
    for path in sorted(glob.glob(r"_local\hashes\hashes.game.txt.*")):
        try:
            handle = open(path, "r", encoding="utf-8", errors="replace")
        except OSError:
            continue
        with handle:
            for line in handle:
                scanned += 1
                parts = line.split()
                if len(parts) < 2:
                    continue
                candidate = None
                for token in parts[:2]:
                    if not hash_re.match(token):
                        candidate = token
                        break
                if candidate is None:
                    continue
                lowered = candidate.lower()
                if extensions and not any(lowered.endswith(e) for e in extensions):
                    continue
                if not all(m.search(candidate) for m in matchers):
                    continue
                found.setdefault(candidate, 0)
                found[candidate] += 1

    items = sorted(found.items())
    print(f"{scanned} lines scanned, {len(items)} unique matches")
    for path, count in items[: args.limit]:
        print(f"  {path}")
    if len(items) > args.limit:
        print(f"  ... {len(items) - args.limit} more")

    suffixes = {}
    for path, _ in items:
        suffix = path.rsplit(".", 1)[-1].lower() if "." in path else "(none)"
        suffixes[suffix] = suffixes.get(suffix, 0) + 1
    if suffixes:
        print("\nextensions:", ", ".join(f"{k}={v}" for k, v in
                                          sorted(suffixes.items(), key=lambda kv: -kv[1])))


if __name__ == "__main__":
    main()
