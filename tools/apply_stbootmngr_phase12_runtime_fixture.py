#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime
import shutil

ROOT = Path(__file__).resolve().parents[1]
path_fixture = ROOT / "tests/stbootmngr_apfs_path_selftest.cpp"
base_fixture = ROOT / "tests/stbootmngr_basesystem_selftest.cpp"

edits = [
    (path_fixture,
     "constexpr uint64_t BLOCKS = 16;",
     """#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t BLOCKS = 1024;
static_assert(BS * BLOCKS <= 4ULL * 1024ULL * 1024ULL,
              "Phase 12 APFS fixture exceeds the 4 MiB test capacity");
#else
constexpr uint64_t BLOCKS = 16;
#endif"""),
    (base_fixture,
     '#include "stbootmngr_apfs_file_fixture.hpp"',
     '''#ifdef STBOOT_PHASE12_LARGE_FIXTURE
#include "stbootmngr_phase12_apfs_fixture.hpp"
#else
#include "stbootmngr_apfs_file_fixture.hpp"
#endif'''),
    (base_fixture,
     "constexpr uint64_t E2E_DMG_CAPACITY = 128ULL * 1024ULL;",
     """#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t E2E_DMG_CAPACITY = 5ULL * 1024ULL * 1024ULL;
#else
constexpr uint64_t E2E_DMG_CAPACITY = 128ULL * 1024ULL;
#endif"""),
    (base_fixture,
     "constexpr uint64_t E2E_RXFS_BYTES = 4ULL * 1024ULL * 1024ULL;",
     """#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t E2E_RXFS_BYTES = 16ULL * 1024ULL * 1024ULL;
#else
constexpr uint64_t E2E_RXFS_BYTES = 4ULL * 1024ULL * 1024ULL;
#endif"""),
]

all_applied = True
for path, old, new in edits:
    if not path.is_file():
        raise SystemExit(f"ABORT: missing required source: {path}")
    if new not in path.read_text():
        all_applied = False
if all_applied:
    print("Phase 12 runtime fixture integration is already applied; no changes made.")
    raise SystemExit(0)

# Stage edits per file so multiple changes to one file accumulate.
# Skip edits already present, making the patcher safely repeatable.
updated = {}
for path, old, new in edits:
    content = updated.get(path, path.read_text())
    if new in content:
        updated[path] = content
        continue
    if content.count(old) != 1:
        raise SystemExit(
            f"ABORT: expected one untouched anchor in {path}; found {content.count(old)}. "
            "No source changes made."
        )
    updated[path] = content.replace(old, new, 1)

# Avoid rewriting or backing up files which did not change.
updated = {
    path: content for path, content in updated.items()
    if content != path.read_text()
}
if not updated:
    print("Phase 12 runtime fixture integration is already applied; no changes made.")
    raise SystemExit(0)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backups = []
for path in updated:
    backup = path.with_name(path.name + f".bak-phase12-runtime-fixture-{stamp}")
    shutil.copy2(path, backup)
    backups.append(backup)
for path, content in updated.items():
    path.write_text(content)

print("Phase 12 runtime fixture integration applied.")
for backup in backups:
    print("Backup:", backup)
print("Only conditional test-fixture geometry/capacity changes were made.")
