History fetching fix
===================

Observed causes in the original code
-----------------------------------
* fetchAll() always began at after=0, even after loading existing IndexedDB
  records. Every manual fetch and 30-second poll downloaded retained base history.
* Each /api/history page opened the CSV at byte zero and scanned past every
  preceding record. For N records and 100-record pages, a complete download
  visited roughly N squared / 200 records, plus the returned records.
* readStringUntil() read through Stream one character at a time and could wait
  for a timeout on a torn final line. Flash access and String allocation occurred
  for skipped records as well as returned records.
* Rendering happened only after all pages completed. Database failures prevented
  startup/polling entirely. A page shorter than 100 was treated as completion,
  although the server also has a response-byte limit.

The code establishes these causes, but does not establish which accounts for
how much of the reported 6–7 seconds. Device latency has not been measured here.

Changes and expected performance
--------------------------------
Dashboard loads its cache, then validates the dataset and prunes records through
the base retained-record boundary before displaying it. Metadata selects the current
base dataset; history requests start at the maximum committed record ID. Each
page commits before advancing the durable cursor and then updates the display.
A failed write switches to explicitly labelled memory-only operation: a separate
session cursor avoids repeat downloads in that tab; reload retries from durable
storage. Requests have a 20-second timeout; errors release the busy guard for
retry. Manual fetch and polling use the same guard, including during latest-data
loading. Browser archive acknowledgment is serialized with fetching and requires a
successful history fetch. The Node archive app provides the weekly archive workflow;
see archive-app/README.md. Both use dataset-checked acknowledgments.

/api/latest supplies the latest retained/received-in-this-boot reading per sensor
before backfill, without contributing to the history cursor. It requires no
filesystem scan. Older firmware returning 404 falls back to history. Cards use
all known readings, independently of the chart period. The overdue threshold is
editable (default 360 seconds for approximately 120-second reporting). The log
contains measurement timestamps, NOT base receipt timestamps: the UI explicitly
shows measurement time/age. Buffered uploads can therefore appear overdue even
when just received. Zero timestamps show unknown age. There is no configured
expected-sensor list, so nonexistent sensors are not invented.

Firmware retains the existing CSV, upload protocol and 100-record history format,
adding dataset fields, /api/latest and archive protocol 2 metadata. Archive
acknowledgments now require a matching dataset query parameter. A 128-entry sparse index is
built during the existing boot scan, thinned as the log grows and updated during
uploads. Eight continuation offsets serve sequential pages directly. Buffered
512-byte reads avoid character-by-character filesystem calls. Indexes are rebuilt
after successful archive compaction and cleared on explicit reset. A partial
last line is not served as JSON; its stored bytes are left untouched.

The index, continuation cache and latest table consume a fixed 6,208 bytes total,
plus counters, a 512-byte read buffer, a 128-byte line buffer and the bounded
history response (~12 KB). Latest responses stream one sensor at a time. History
length does not increase these buffers. Single-client sequential backfill should
read approximately the log once instead of repeatedly reading all preceding
pages. On cache misses, seek uses the preceding sparse checkpoint; the distance
between checkpoints grows with history size. LittleFS seek costs still apply.

Remaining work is proportional to newly fetched history. Boot recovery and
archive compaction still scan the retained log, and synchronous upload flush/NVS
writes or compaction can delay HTTP service. Browser history remains in memory
for charts/export, as before; very large all-history charts can still be costly.
Latest cached values covered by an archive acknowledgment are cleared, so
/api/latest reflects retained readings rather than already archived history.

Browser migration
-----------------
No database is deleted. After a verified archive, current-dataset browser samples
through the retained-record boundary are pruned to keep the live view current.
The original samples store and IDs are unchanged; the
schema advances from version 1 to 2 to add an ownership metadata store. Existing
localStorage dataset ownership is adopted once. New datasets use separate
IndexedDB databases and dataset-specific archive cursors. Old dataset history
remains stored, although it is not included in the active dataset's charts/CSV.
It is accessible through browser developer tools (Application > IndexedDB).
If ownership was already lost, unassigned legacy records are retained separately
rather than guessing that reused IDs belong to the current base.

Normally only reload the dashboard after flashing. Do not clear site data.
Close old dashboard tabs if an upgrade is blocked, then reload; a blocked or
unavailable database uses memory-only mode with a visible warning. Explicitly
confirmed Reset retains its deliberately destructive semantics for the current
browser dataset; automatic detection of a reset preserves previous datasets.

Validation
----------
Run from the repository root:
  python3 scripts/embed_dashboard.py
  node --test tests/archive.test.mjs tests/dashboard.test.cjs
  python3 tests/firmware_index_test.py
  git diff --check

The dashboard Node tests execute the actual dashboard script with a DOM stub and an
asynchronous transaction mock (not a real browser IndexedDB engine). They cover
empty and populated databases, repeated fetch/reload, interrupted backfill,
noncontiguous IDs, overlap guards, HTTP failure/retry, failed storage commits,
unavailable/blocked storage, late database-open success, immediate latest cards,
dataset changes, mid-download dataset changes, unknown legacy ownership,
localStorage restrictions, malformed pagination and embedded-page consistency.

The C++ host test compiles the actual firmware reader/index functions, checking
a million sparse IDs, fixed array sizes, checkpoint placement, buffered reading,
oversized lines and torn final lines. It does not compile the complete Arduino
sketch or model LittleFS power-loss behaviour. Arduino CLI/ESP32 board packages
and physical hardware were not available, so full firmware compilation, real
browser visual verification, flash latency and live uploads remain unverified.

Manual verification on the base
-------------------------------
1. Back up the existing CSV and record the current /api/meta dataset. Build for
   the actual ESP32 board and its existing partition scheme. Flash only the
   application; do not erase flash, format LittleFS or upload a filesystem image.
2. Confirm the dataset and existing history survived, and ordinary sensor uploads
   continue to receive ACKs. Check /api/latest returns valid JSON promptly.
3. Reload an existing browser profile. In Network, confirm the first history
   query uses its highest cached ID. Repeat Fetch and wait for an automatic poll:
   neither should restart from zero. Confirm charts, filters and CSV export.
4. In a fresh profile, confirm latest cards appear before backfill completes,
   progress increases per page, and clicking Fetch cannot overlap a download.
   Reload partway through and verify the first cursor resumes committed history.
5. Temporarily disconnect Wi-Fi or block a history request. Check the error and
   successful retry. Simulate storage denial/quota failure in a disposable profile;
   verify the warning, current cards, and refetch of uncommitted rows on reload.
6. On a disposable base/dataset only, test reset detection and archive compaction.
   Confirm previous browser databases remain after an externally triggered reset;
   verify pending archive acknowledgments cannot cross dataset boundaries.
7. Compare DevTools request timing at low/high record IDs before and after this
   build, with the same log and network. Record time-to-first-card, complete
   backfill time, and second-fetch request count. Check free heap over repeated
   requests and while uploading. Avoid claiming a speedup from host tests alone.

Deployment
----------
The complete fix requires rebuilding/flashing the base station, which embeds
base_station/dashboard_page.h and provides the optimized API and /api/latest.
Updating a separately served dashboard.html alone supplies the browser fixes
with old-API fallback, but leaves firmware scan costs. Sensor nodes need no
changes or reflashing. The generated header must match dashboard.html; use the
included generation script after future edits.
