#!/usr/bin/env python3
"""Check that all #hex error codes are unique and all thrown string literals have one."""

from collections import defaultdict
from pathlib import Path
import re
import secrets
import sys

PRUNE_DIRS = {
    ".git",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "third_party",
    "external",
}
EXTENSIONS = {".cpp", ".hpp"}

# Matches standard C++ string literals (handling escaped quotes like \")
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')

# Matches error strings starting with #<6-hex-digits> (e.g., #39d522: ...)
ERROR_CODE_RE = re.compile(r"^#([0-9a-fA-F]{6})\b")

# Matches throw statements taking a direct string literal, e.g.:
#   throw std::runtime_error("...");
#   throw CustomException("...");
THROW_LITERAL_RE = re.compile(r'\bthrow\s+[\w:]+\s*\(\s*"((?:\\.|[^"\\])*)"')


def iter_source_files(root: Path):
  for path in root.rglob("*"):
    if any(part in PRUNE_DIRS for part in path.parts):
      continue
    if path.is_file() and path.suffix in EXTENSIONS:
      yield path


def generate_unique_hex(used_codes: set[str], length: int = 6) -> str:
  while True:
    candidate = secrets.token_hex((length + 1) // 2)[:length].lower()
    if candidate not in used_codes:
      used_codes.add(candidate)
      return candidate


def main() -> int:
  root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")

  # Maps lowercase hex code -> list of (filepath, line_num, full_message)
  occurrences: dict[str, list[tuple[Path, int, str]]] = defaultdict(list)
  # List of (filepath, line_num, full_message) for throw statements missing a valid code
  missing_codes: list[tuple[Path, int, str]] = []

  for filepath in sorted(iter_source_files(root)):
    content = filepath.read_text(encoding="utf-8", errors="replace")
    for line_num, line in enumerate(content.splitlines(), start=1):
      # 1. Collect all #<hex> string literals across the codebase
      for match in STRING_LITERAL_RE.finditer(line):
        literal = match.group(1)
        code_match = ERROR_CODE_RE.match(literal)
        if code_match:
          code = code_match.group(1).lower()
          occurrences[code].append((filepath, line_num, literal))

      # 2. Check that throw statements with string literals start with a valid #<hex>
      for throw_match in THROW_LITERAL_RE.finditer(line):
        literal = throw_match.group(1)
        if not ERROR_CODE_RE.match(literal):
          missing_codes.append((filepath, line_num, literal))

  used_codes = set(occurrences.keys())
  duplicates = {
      code: locs for code, locs in occurrences.items() if len(locs) > 1
  }
  has_errors = False

  if duplicates:
    has_errors = True
    print(
        f"ERROR: Found {len(duplicates)} duplicate error code(s):\n",
        file=sys.stderr,
    )
    for code, locs in sorted(duplicates.items()):
      print(f"Duplicate code #{code}:", file=sys.stderr)
      for idx, (filepath, line_num, message) in enumerate(locs):
        print(
            f'  [{idx + 1}] {filepath}:{line_num} -> "{message}"',
            file=sys.stderr,
        )
        if idx > 0:
          suggestion = generate_unique_hex(used_codes, length=len(code))
          print(f"      Suggested replacement: #{suggestion}", file=sys.stderr)
      print(file=sys.stderr)

  if missing_codes:
    has_errors = True
    print(
        f"ERROR: Found {len(missing_codes)} throw statement(s) missing a valid"
        " #hex prefix:\n",
        file=sys.stderr,
    )
    for filepath, line_num, message in missing_codes:
      suggestion = generate_unique_hex(used_codes)
      print(f'  {filepath}:{line_num} -> "{message}"', file=sys.stderr)
      print(
          f'      Suggested prefix: "#{suggestion}: {message}"', file=sys.stderr
      )
    print(file=sys.stderr)

  if has_errors:
    return 1

  print(f"OK: All {len(occurrences)} error codes are unique and properly formatted.")
  return 0


if __name__ == "__main__":
  sys.exit(main())