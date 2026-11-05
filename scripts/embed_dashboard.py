"""Regenerate the dashboard embedded in the base-station sketch."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
html = (root / 'dashboard.html').read_text()
assert ')ESPHTML"' not in html
(root / 'base_station/dashboard_page.h').write_text(
    '#pragma once\n#include <pgmspace.h>\nstatic const char DASHBOARD_PAGE[] PROGMEM = R"ESPHTML('
    + html + ')ESPHTML";\n')
