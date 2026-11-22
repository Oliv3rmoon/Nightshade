#!/usr/bin/env python3
"""Compare game frame rates and explain a Vulkan development run's GPU timings.

The Vulkan (EDEN_VULKAN_FRAME) and OpenGL (EDEN_GAME_FRAME) reports in an
ordinary heap.log cover one window each. Use one log per gameplay run.
Development GPU counters are cumulative, so their rates use differences
between consecutive reports.

    tools/analyze-perf-log.py heap.log            one run
    tools/analyze-perf-log.py --windows heap.log  the per-window table too
    tools/analyze-perf-log.py a.log b.log         two runs side by side
"""
import math
import re
import sys
from pathlib import Path

TOKEN = re.compile(r'\b(EDEN_[A-Z0-9_]+)')
FIELD = re.compile(r'([A-Za-z_]\w*)=([^\s]+)')
FRAME_REPORTS = {'EDEN_VULKAN_FRAME': 'Vulkan', 'EDEN_GAME_FRAME': 'OpenGL'}

# Cumulative counters (ReportGpuThread's "Cumulative totals; the analyzer takes
# deltas between consecutive reports"). Everything else printed inside a window
# is already a window's worth or a point-in-time snapshot.
CUMULATIVE = {
    'EDEN_DEV_GPU': 'gpu',
    'EDEN_DEV_DRAW': 'draw',
    'EDEN_DEV_PUSH': 'push',
    'EDEN_DEV_RAST': 'rast',
    'EDEN_DEV_CFG': 'cfg',
    'EDEN_DEV_BIND': 'bind',
    'EDEN_DEV_GEO': 'geo',
    'EDEN_DEV_UPD': 'upd',
    'EDEN_DEV_GC_DRAIN': 'gc',
    'EDEN_DEV_GUEST': 'guest',
}
SNAPSHOT = {'EDEN_PERF_DIRECT': 'direct'}

# The six phases of RasterizerVulkan::PrepareDraw, in the order they run.
PHASES = ('flush', 'pipeline', 'lock_wait', 'config', 'state', 'record')
# The six inside config, all of which but SetEngine is GraphicsPipeline::Configure.
CFG_PHASES = ('sync', 'read', 'bind', 'desc', 'rt', 'draw')
# The three inside cfg_bind: the texture-buffer loop, the buffer update pass, the host binds.
BIND_PHASES = ('tbo', 'update', 'geometry')
# The four calls inside bind_geometry, and the three inside bind_update.
GEO_PHASES = ('index', 'vertex', 'xf', 'indirect')
UPD_PHASES = ('state', 'stage', 'indirect')

# Every group that nests under one parent, and how to print it: the row prefix its phases are
# stored under, the key prefix derive() writes per-draw microseconds to, the parent's row key,
# a title, and what the unattributed row is made of.
GROUPS = (
    ('cfg', CFG_PHASES, 'phase_cfg_', 'rast.config_ns',
     'Configure (all of config but SetEngine)',
     'entry clock, SetEngine, the pointer call; negative is mark overhead'),
    ('bind', BIND_PHASES, 'phase_bind_', 'cfg.bind_ns',
     'cfg_bind', 'transform-feedback check, two pointer stores, the clock itself'),
    ('geo', GEO_PHASES, 'phase_geo_', 'bind.geometry_ns',
     'bind_geometry, the host binds', 'the clock and the two marks around it'),
    ('upd', UPD_PHASES, 'phase_upd_', 'bind.update_ns',
     'bind_update, the buffer update pass', 'the clock and the two marks around it'),
)


def number(value):
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return None


def parse(path):
    events = []
    for line in path.read_text(errors='replace').splitlines():
        match = TOKEN.search(line)
        if not match:
            continue
        values = {key: number(raw) if number(raw) is not None else raw
                  for key, raw in FIELD.findall(line[match.start():])}
        kind = match.group(1)
        if values or kind in FRAME_REPORTS:
            events.append((kind, values))
    return events


def split_windows(events):
    """Every event up to the next frame report belongs to the window it opens."""
    windows, current = [], None
    for kind, values in events:
        if kind in FRAME_REPORTS:
            if current is not None:
                windows.append(current)
            current = {'frame': values, 'renderer': FRAME_REPORTS[kind], 'groups': {}}
        elif current is not None:
            current['groups'].setdefault(kind, []).append(values)
    if current is not None:
        windows.append(current)
    return windows


def build_rows(windows):
    rows, previous = [], {}
    for index, window in enumerate(windows):
        row = dict(window['frame'])
        row['index'] = index
        row['renderer'] = window['renderer']
        groups = window['groups'] if window['renderer'] == 'Vulkan' else {}
        for kind, values in groups.items():
            if kind not in CUMULATIVE:
                continue
            raw = values[0]
            base = previous.get(kind)
            if base is not None:
                for key, value in raw.items():
                    before = base.get(key)
                    if not isinstance(value, (int, float)) or not isinstance(before, (int, float)):
                        continue
                    step = value - before
                    # A decrease is the counters starting over, not a negative interval.
                    row[f'{CUMULATIVE[kind]}.{key}'] = step if step >= 0 else value
            previous[kind] = raw
        for kind, values in groups.items():
            if kind in SNAPSHOT:
                for key, value in values[-1].items():
                    row[f'{SNAPSHOT[kind]}.{key}'] = value
        derive(row)
        rows.append(row)
    return rows


def derive(row):
    """Rates a report window can stand on its own. Window 0 has no deltas, so it gets none."""
    gpu_frame = row.get('gpu.frame')
    draws = row.get('draw.calls')
    dispatch = row.get('gpu.dispatch_ns')
    rast = row.get('draw.ns')
    if gpu_frame and draws and dispatch and rast:
        row['dispatch_ms'] = dispatch / 1e6
        row['rast_ms'] = rast / 1e6
        row['disp_us_per_frame'] = dispatch / gpu_frame / 1000.0
        row['rast_us_per_draw'] = rast / draws / 1000.0
        row['decode_us_per_draw'] = (dispatch - rast) / draws / 1000.0
        row['draws_per_frame'] = draws / gpu_frame
        row['present_us_per_frame'] = row.get('gpu.present_ns', 0) / gpu_frame / 1000.0
        for phase in PHASES:
            spent = row.get(f'rast.{phase}_ns')
            if spent is not None:
                row[f'phase_{phase}_us'] = spent / draws / 1000.0
        for prefix, phases, key, _total, _title, _foot in GROUPS:
            for phase in phases:
                spent = row.get(f'{prefix}.{phase}_ns')
                if spent is not None:
                    row[f'{key}{phase}_us'] = spent / draws / 1000.0
        unattributed = row.get('rast.unattributed_ns')
        if unattributed is not None and rast:
            row['unattributed_frac'] = unattributed / rast
    if gpu_frame:
        for key, name in (('guest.ipc_ns', 'ipc_ms'),
                          ('guest.cache_lock_contended', 'lockcont_per_frame'),
                          ('guest.cache_lock_blocked', 'lockblocked_per_frame'),
                          ('guest.cpu_read_ns', 'cpu_read_ms')):
            if row.get(key) is not None:
                row[name] = row[key] / gpu_frame / (1e6 if name.endswith('_ms') else 1)
    return row


def percentile(values, portion):
    ordered = sorted(values)
    if not ordered:
        return float('nan')
    position = (len(ordered) - 1) * portion
    low, high = math.floor(position), math.ceil(position)
    if low == high:
        return ordered[int(position)]
    return ordered[low] * (high - position) + ordered[high] * (position - low)


def summarize(values):
    return {'min': min(values), 'p25': percentile(values, 0.25),
            'med': percentile(values, 0.5), 'p75': percentile(values, 0.75),
            'max': max(values), 'mean': sum(values) / len(values)}


def pearson(xs, ys):
    if len(xs) < 3:
        return float('nan')
    mean_x, mean_y = sum(xs) / len(xs), sum(ys) / len(ys)
    sxy = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    sxx = sum((x - mean_x) ** 2 for x in xs)
    syy = sum((y - mean_y) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return float('nan')
    return sxy / math.sqrt(sxx * syy)


# Columns worth printing for one run: fps first, then the dispatch both ways round -
# the wall time it took (dispatch_ms, rast_ms) and what it costs per unit of work
# (us per frame, us per draw), since only the first of those moves with fps.
CORE = ('fps', 'dispatch_ms', 'rast_ms', 'disp_us_per_frame', 'rast_us_per_draw',
        'decode_us_per_draw', 'draws_per_frame', 'lockcont_per_frame', 'ipc_ms')


def report(rows, path, show_windows):
    usable = [row for row in rows if 'disp_us_per_frame' in row]
    if not rows or any(not isinstance(row.get('frames'), int) or row['frames'] < 0 or
                       not isinstance(row.get('seconds'), (int, float)) or
                       not math.isfinite(row['seconds']) or row['seconds'] <= 0
                       for row in rows):
        print(f'{path}: expected complete Vulkan or OpenGL frame reports', file=sys.stderr)
        return None
    renderer = rows[0]['renderer']
    if any(row['renderer'] != renderer for row in rows[1:]):
        print(f'{path}: mixed Vulkan and OpenGL reports; use one log per run', file=sys.stderr)
        return None
    totals = [row['total'] for row in rows if isinstance(row.get('total'), int)]
    if any(current <= previous for previous, current in zip(totals, totals[1:])):
        print(f'{path}: frame totals restart or repeat; use one log per run', file=sys.stderr)
        return None

    seconds = sum(row['seconds'] for row in rows)
    frames = sum(row['frames'] for row in rows)
    fps = [row['frames'] / row['seconds'] for row in rows]
    stats = summarize(fps)
    stats['mean'] = frames / seconds
    stats['under_30_s'] = sum(row['seconds'] for row, rate in zip(rows, fps) if rate < 30)
    worst = [row.get('worst_ms') for row in rows]
    if all(isinstance(value, (int, float)) and math.isfinite(value) and value >= 0
           for value in worst):
        stats['worst_ms'] = max(worst)
    summary = {'fps': stats}

    print(f'\n{path}  {renderer}, {len(rows)} frame windows, {frames} frames over {seconds:.1f}s')
    print(f"  game FPS  mean {stats['mean']:.2f}  "
          f"window min {stats['min']:.2f}  median {stats['med']:.2f}  max {stats['max']:.2f}")
    print(f"  time in windows under 30 FPS  {stats['under_30_s']:.2f}s")
    if 'worst_ms' in stats:
        print(f"  worst frame  {stats['worst_ms']:.2f} ms")
    if not usable:
        if show_windows:
            print('    win      fps seconds worst_ms')
            for row, rate in zip(rows, fps):
                hitch = row.get('worst_ms')
                hitch_text = f'{hitch:.2f}' if isinstance(hitch, (int, float)) else '-'
                print(f"  {row['index']:>4} {rate:>8.2f} {row['seconds']:>7.2f} {hitch_text:>8}")
        return summary

    present = [name for name in CORE[1:] if any(name in row for row in usable)]
    print('  ' + f'{"metric":>22} ' + '  '.join(f'{key:>6}' for key in
                                                ('min', 'p25', 'med', 'p75', 'max', 'mean', 'r')))
    for name in present:
        values = [row[name] for row in usable if name in row]
        pairs = [(row['fps'], row[name]) for row in usable if 'fps' in row and name in row]
        if not values or not pairs:
            continue
        stats = summarize(values)
        cells = '  '.join(f'{stats[key]:>6.2f}' for key in ('min', 'p25', 'med', 'p75', 'max', 'mean'))
        print('  ' + f'{name:>22} ' + cells + f'  {pearson(*zip(*pairs)):>6.2f}')

    report_phases(usable)
    for prefix, phases, _key, total, title, foot in GROUPS:
        report_group(usable, prefix, phases, total, title, foot)
    if show_windows:
        print('\n    win     fps  disp/f  rast/f  us/draw  draws/f  ipc_ms  lockcont')
        for row in usable:
            if 'fps' not in row:
                continue
            print(f"  {row['index']:>4} {row['fps']:>7.1f} {row['disp_us_per_frame']:>7.0f} "
                  f"{row['rast_ms']:>7.1f} {row['rast_us_per_draw']:>8.2f} "
                  f"{row['draws_per_frame']:>8.1f} {row.get('ipc_ms', float('nan')):>7.1f} "
                  f"{row.get('lockcont_per_frame', 0):>9.0f}")
    for name in present:
        values = [row[name] for row in usable if name in row]
        if values:
            summary[name] = summarize(values)
    for phase in PHASES:
        values = [row[f'phase_{phase}_us'] for row in usable if f'phase_{phase}_us' in row]
        if values:
            summary[f'phase.{phase}'] = summarize(values)
    for prefix, phases, key, _total, _title, _foot in GROUPS:
        for phase in phases:
            values = [row[f'{key}{phase}_us'] for row in usable if f'{key}{phase}_us' in row]
            if values:
                summary[f'phase.{prefix}.{phase}'] = summarize(values)
    return summary


def report_phases(rows):
    """The rasterizer's time, shared out over the six phases of PrepareDraw."""
    if not any(f'rast.{phase}_ns' in row for row in rows for phase in PHASES):
        return  # no EDEN_DEV_RAST in this log - it predates the six-phase split
    spent = {phase: sum(row.get(f'rast.{phase}_ns', 0) for row in rows) for phase in PHASES}
    unattributed = sum(row.get('rast.unattributed_ns', 0) for row in rows)
    total = sum(row.get('draw.ns', 0) for row in rows)
    draws = sum(row.get('draw.calls', 0) for row in rows)
    if not total or not draws:
        return None
    print(f'\n  PrepareDraw, {total / 1e6:.1f} s over {draws:,} draws '
          f'({total / draws / 1000:.2f} us/draw), by phase:')
    for phase in PHASES:
        share = spent[phase] / total * 100
        print(f'    {phase:>10} {spent[phase] / 1e6:>8.1f} s  {share:>5.1f}%  '
              f'{spent[phase] / draws / 1000:>7.3f} us')
    print(f'    {"unattributed":>10} {unattributed / 1e6:>8.1f} s  {unattributed / total * 100:>5.1f}%'
          f'  {unattributed / draws / 1000:>7.3f} us   <- early returns the Timer still charged')


def report_group(rows, prefix, phases, total_key, title, footnote):
    """One nested group's table. Absent entirely when that split predates the log."""
    if not any(f'{prefix}.{phase}_ns' in row for row in rows for phase in phases):
        return  # no such line in this log - it predates that split
    spent = {phase: sum(row.get(f'{prefix}.{phase}_ns', 0) for row in rows) for phase in phases}
    unattributed = sum(row.get(f'{prefix}.unattributed_ns', 0) for row in rows)
    total = sum(row.get(total_key, 0) for row in rows)
    draws = sum(row.get('draw.calls', 0) for row in rows)
    if not total or not draws:
        return
    print(f'\n  {title}, {total / 1e6:.1f} s over {draws:,} draws '
          f'({total / draws / 1000:.2f} us/draw), by phase:')
    for phase in phases:
        print(f'    {phase:>10} {spent[phase] / 1e6:>8.1f} s  {spent[phase] / total * 100:>5.1f}%  '
              f'{spent[phase] / draws / 1000:>7.3f} us')
    print(f'    {"unattributed":>10} {unattributed / 1e6:>8.1f} s  '
          f'{unattributed / total * 100:>5.1f}%  {unattributed / draws / 1000:>7.3f} us'
          f'   <- {footnote}')


def compare(runs):
    names = list(runs)
    print('\n  side by side:')
    print('  ' + f'{"":>24}' + '  '.join(f'{name[-24:]:>24}' for name in names))
    keys = sorted({key for summary in runs.values() for key, value in summary.items()
                   if isinstance(value, dict) and value})
    for key in keys:
        stats = [('mean', key), ('p25', '  p25'), ('med', '  med'), ('p75', '  p75')]
        if key == 'fps':
            stats.extend([('under_30_s', 's in <30 FPS windows'), ('worst_ms', 'worst frame ms')])
        for statistic, label in stats:
            cells = []
            for name in names:
                value = runs[name].get(key, {}).get(statistic)
                cells.append('-' if value is None else f'{value:.2f}')
            print(f'  {label:>24} ' + '  '.join(f'{cell:>24}' for cell in cells))


def main(argv):
    paths, show_windows = [], False
    for argument in argv:
        if argument == '--windows':
            show_windows = True
        else:
            paths.append(argument)
    if not paths:
        paths = ['heap.log']
    runs = {}
    for path in paths:
        file = Path(path)
        if not file.is_file():
            print(f'{path}: no such file', file=sys.stderr)
            return 1
        summary = report(build_rows(split_windows(parse(file))), path, show_windows)
        if summary is None:
            return 1
        runs[path] = summary
    if len(runs) > 1:
        compare(runs)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
