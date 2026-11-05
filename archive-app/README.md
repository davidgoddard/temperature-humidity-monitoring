# Weekly sensor archives

Requires Node.js 22 or later. No npm install is needed. Run these commands from the repository root.

## One-time base update

Rebuild and flash `base_station/base_station.ino` with the updated embedded dashboard. Upload the application only: preserve the LittleFS filesystem and existing readings. Sensor firmware does not need changing. The archive command refuses older base firmware because it cannot safely condition deletion on the dataset identifier.

The updated base advertises `archive_protocol: 2`, `latest_id`, and `retained_after` in `/api/meta`. Archive acknowledgements must include `?dataset=<dataset identifier>`; the base rejects a mismatch before deleting anything.

## Run once a week

```sh
node archive-app/app.mjs archive
```

This downloads a bounded snapshot, saves a dated CSV in `archive-app/archives/<dataset>/`, flushes and verifies the file, and then acknowledges those record IDs. Only acknowledged records are removed from the base. New uploads remain for next time. Nothing resets sensor sequences or record IDs. There is no need to leave Node running between weekly runs.

For a different base address or storage folder:

```sh
node archive-app/app.mjs archive --base http://192.168.1.50 --dir "/path/to/Sensor archives"
```

Use the same directory each week. Keep a backup of this directory: after acknowledgement these files are the long-term copy. The app checks filesystem writes and flushes; hardware and filesystem durability guarantees still apply. A local disk is preferable to a network or cloud-mounted folder; back it up separately.

No weekly schedule is installed automatically. You can run the command manually or schedule it with your OS while the computer is awake and on the base's network.

## View the entire history

```sh
node archive-app/app.mjs serve
```

Open http://127.0.0.1:8080. For custom storage use the same `--dir` as the archive command; `--port 8081` selects another port. The server listens only on this computer. The base does not need to be online to view saved archives. Stop with Ctrl+C.

The viewer loads all CSVs inside the dataset subdirectories, merges overlapping records, and offers local start/end times, sensor and dataset filters, temperature/humidity charts, and CSV export of the selected period. Click **Reload archive files** after another archive run. It keeps different datasets separate even when a base reset reused record IDs. Conflicting duplicates or malformed files produce an error rather than silently dropping data. Unknown timestamps appear in the all-dates table and export but cannot be plotted or assigned to a date range. Lines break across gaps longer than one hour.

Archive files use the existing six-column CSV format. Preserve the dataset folder names when copying or restoring archives. Older browser downloads can be placed inside the correct dataset folder if you know which dataset they belong to; the old CSV format does not contain that identity. Do not combine unrelated resets in one folder. Viewer exports contain an extra dataset column and are intended for external analysis, rather than re-import as archive files.

All files are loaded into memory; very large histories may take longer to load and draw. The table shows the latest 100 matching readings; the graph and export use the full selection.

## Failures and retries

- A download, validation, or local write failure does not acknowledge the base.
- If acknowledgement fails, the saved CSV remains. Rerun the command. Any overlapping files are combined by dataset and record ID in the viewer.
- If the acknowledgement actually succeeded but its response was lost, a rerun collects only remaining readings (or reports no readings).
- A reset or another archive operation during downloading aborts the run. Retry against the current base state.
- `.archive.lock` prevents overlapping runs in the same archive directory. If the process is killed, first confirm no archive process is still running, then remove that lock file and rerun. Incomplete `.tmp` files are ignored by the viewer.
- Avoid running other archivers against the same base at the same time. Use this app as the weekly archiver.

## Live base dashboard

On the updated firmware, the base dashboard prunes its browser cache using the base's retained-record boundary. Archived readings disappear at the next successful refresh (normally within 30 seconds). It does not show old browser history at startup before connecting. Older datasets may remain separately stored in IndexedDB but are not included in the active live view. Historical browsing belongs in this Node viewer.

## Verification

```sh
node --test tests/archive.test.mjs tests/dashboard.test.cjs
python3 tests/firmware_index_test.py
```

These are automated tests using a simulated base and host C++ reader checks. Actual ESP32 compilation, flashing, and hardware/power-loss behaviour must be verified on the board.
