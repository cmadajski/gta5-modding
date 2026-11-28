#!/usr/bin/env python3
"""Compare record-time GTA wheel transforms with the prior captured draw."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re


MARKER = b'[NativeWheelTrace]'
CAPTURE_NAME = re.compile(r'(NativeWheel-\d+-\d+-\d+)-\d+\.json')
FIELD = re.compile(r'([a-z0-9_]+)=(\S+)')
DRAW_DECIMAL = ('mip', 'layer', 'count', 'instances', 'first', 'base_vertex', 'first_instance',
                'ib_offset', 'cbv_offset', 'cbv_size')
DRAW_HEX = ('vs', 'ps', 'command', 'rt4', 'depth', 'ib', 'table', 'cbv')
DRAW_FLAGS = ('same_list', 'same_motion', 'indirect', 'layout_verified', 'cpu_snapshot')
REGISTRY_FIELDS = ('buffers', 'views', 'descriptors', 'buffer_overflows', 'view_overflows',
                   'descriptor_overflows', 'descriptor_evictions')
MATRIX_NAMES = ('world', 'current_wvp', 'stock_previous_wvp')
RIGID_SHADERS = {'0xdf51595b', '0x2c667625'}
GEOMETRY_FIELDS = ('vs', 'ib', 'ib_offset', 'count', 'instances', 'first', 'base_vertex',
                   'first_instance', 'viewport')


def integer(text, base=10, minimum=0, maximum=(1 << 64) - 1):
    pattern = r'-?\d+' if base == 10 else r'[0-9a-fA-F]+'
    if not re.fullmatch(pattern, text) or len(text) > 21:
        raise ValueError(f'invalid base-{base} integer')
    value = int(text, base)
    if not minimum <= value <= maximum:
        raise ValueError('integer is outside its expected range')
    return value


def finite_values(text, count):
    values = tuple(float(value) for value in text.split(','))
    if len(values) != count or not all(math.isfinite(value) and abs(value) <= 3.403e38 for value in values):
        raise ValueError(f'expected {count} finite float32 values')
    return values


def fields(text):
    pairs = [FIELD.fullmatch(token) for token in text.split()]
    if any(pair is None for pair in pairs):
        raise ValueError('malformed field')
    result = {pair[1]: pair[2] for pair in pairs}
    if len(result) != len(pairs):
        raise ValueError('repeated field name')
    return result


def parse_draw(data, line):
    result = {'line': line, 'draw': integer(data['draw'], maximum=100000)}
    for name in DRAW_DECIMAL:
        result[name] = integer(data[name], minimum=-(1 << 31) if name == 'base_vertex' else 0)
    for name in DRAW_HEX:
        result[name] = f'0x{integer(data[name], base=16):x}'
    if 'rt4_view' in data:
        result['rt4_view'] = f"0x{integer(data['rt4_view'], base=16):x}"
    for name in DRAW_FLAGS:
        result[name] = bool(integer(data[name], maximum=1))
    result['viewport'] = finite_values(data['viewport'], 4)
    return result


def parse_trace(path):
    source_hash = hashlib.sha256()
    dispatches, issues = {}, []
    session = 0
    recognized_lines = 0
    with path.open('rb') as stream:
        for line, raw in enumerate(stream, 1):
            source_hash.update(raw)
            if MARKER not in raw:
                continue
            recognized_lines += 1
            text = raw.split(MARKER, 1)[1].strip().decode('ascii', errors='replace')
            if text.startswith('Tracking enabled'):
                session += 1
                continue
            if not text.startswith('dispatch='):
                continue
            frame = None
            try:
                data = fields(text)
                dispatch = integer(data['dispatch'], minimum=1)
                frame = dispatches.setdefault((session, dispatch), {
                    'session': session, 'dispatch': dispatch, 'summary': None,
                    'draws': {}, 'matrices': {}, 'issues': [],
                })
                if 'matrix' in data:
                    draw = integer(data['draw'], maximum=100000)
                    slot = integer(data['matrix'], maximum=len(MATRIX_NAMES) - 1)
                    key = draw, MATRIX_NAMES[slot]
                    if key in frame['matrices']:
                        raise ValueError(f'duplicate matrix row for draw {draw}')
                    frame['matrices'][key] = {'line': line, 'values': finite_values(data['values'], 16)}
                elif 'draw' in data:
                    draw = parse_draw(data, line)
                    if draw['draw'] in frame['draws']:
                        raise ValueError(f"duplicate draw {draw['draw']}")
                    frame['draws'][draw['draw']] = draw
                elif 'depth_copy' in data:
                    # Copy provenance is separate from draw-matrix diagnostics.
                    continue
                else:
                    if frame['summary'] is not None:
                        raise ValueError('duplicate dispatch summary')
                    frame['summary'] = {
                        'line': line,
                        **{name: f'0x{integer(data[name], base=16):x}' for name in ('fsr_command', 'fsr_motion')},
                        **{name: integer(data[name]) for name in ('candidates', 'recorded', 'omitted')},
                        'descriptor_bounds_ok': bool(integer(data['descriptor_bounds_ok'], maximum=1)),
                        **{name: integer(data[name]) for name in REGISTRY_FIELDS if name in data},
                    }
            except (KeyError, ValueError) as error:
                target = issues if frame is None else frame['issues']
                target.append(f'Line {line}: {error}')
    return list(dispatches.values()), issues, recognized_lines, source_hash.hexdigest()


def load_captures(directory):
    captures, issues = defaultdict(list), []
    if directory is None:
        return captures, issues
    integer_keys = ('dispatch_id', 'context_id', 'sequence', 'recorded_present_interval',
                    'submitted_present_interval', 'submissions', 'render_width', 'render_height',
                    'upscale_width', 'upscale_height')
    flag_keys = ('source_same_dispatch', 'recording_replaced', 'reset')
    for path in sorted(directory.glob('NativeWheel-*.json')):
        match = CAPTURE_NAME.fullmatch(path.name)
        if match is None:
            continue
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
            if not isinstance(data, dict):
                raise ValueError('metadata must be an object')
            for key in integer_keys:
                minimum = 0 if key in ('submissions', 'recorded_present_interval', 'submitted_present_interval') else 1
                if type(data.get(key)) is not int or not minimum <= data[key] < 1 << 64:
                    raise ValueError(f'invalid {key}')
            for key in flag_keys:
                if type(data.get(key)) is not bool:
                    raise ValueError(f'invalid {key}')
            captures[data['dispatch_id']].append({
                'file': path.name, 'burst': match[1], **{key: data[key] for key in (*integer_keys, *flag_keys)},
            })
        except (OSError, ValueError) as error:
            issues.append(f'{path.name}: {error}')
    return captures, issues


def projection(matrix, viewport):
    """Return origin, clip W, and the screen differential of each local basis vector."""
    x, y, width, height = viewport
    w = matrix[15]
    if w <= 1e-5 or width <= 0 or height <= 0:
        return None
    ndc_x, ndc_y = matrix[12] / w, matrix[13] / w
    origin = (x + (ndc_x + 1) * width / 2, y + (1 - ndc_y) * height / 2)
    axes = tuple(((matrix[i] - ndc_x * matrix[i + 3]) * width / (2 * w),
                  -(matrix[i + 1] - ndc_y * matrix[i + 3]) * height / (2 * w))
                 for i in (0, 4, 8))
    return {'origin_px': origin, 'clip_w': w, 'axes_px_per_unit': axes}


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def compare_axes(reference, target):
    """Fit a rotation only in local Y/Z, the tire plane of the rigid shader."""
    x, y, z = reference
    tx, ty, tz = target
    cosine = dot(y, ty) + dot(z, tz)
    sine = dot(z, ty) - dot(y, tz)
    normalizer = max(math.hypot(*y, *z), math.hypot(*ty, *tz))
    if normalizer <= 1e-12:
        return None
    angle = math.atan2(sine, cosine)
    c, s = math.cos(angle), math.sin(angle)
    rotated_y = tuple(c * a + s * b for a, b in zip(y, z))
    rotated_z = tuple(-s * a + c * b for a, b in zip(y, z))
    return {
        'roll_fit_degrees': math.degrees(angle),
        'roll_fit_relative_error': math.hypot(math.dist(rotated_y, ty), math.dist(rotated_z, tz)) / normalizer,
        'plane_relative_difference': math.hypot(math.dist(y, ty), math.dist(z, tz)) / normalizer,
        'axle_relative_difference': math.dist(x, tx) / normalizer,
    }


def compare_projection(reference, target):
    return {
        'origin_distance_px': math.dist(reference['origin_px'], target['origin_px']),
        'clip_w_relative_difference': abs(reference['clip_w'] - target['clip_w']) /
        max(reference['clip_w'], target['clip_w']),
        'projected_basis': compare_axes(reference['axes_px_per_unit'], target['axes_px_per_unit']),
    }


def world_axes(draw):
    matrix = draw['matrices']['world']['values']
    return tuple(matrix[i:i + 3] for i in (0, 4, 8))


def prepare_frame(frame, captures):
    attached = captures.get(frame['dispatch'], [])
    frame['capture'] = attached[0] if len(attached) == 1 else None
    if len(attached) > 1:
        frame['issues'].append('More than one capture metadata file has this dispatch ID.')
    summary = frame['summary']
    if summary is None:
        frame['issues'].append('Dispatch summary is missing.')
    elif summary['recorded'] != len(frame['draws']):
        frame['issues'].append('Dispatch summary and available draw count disagree.')

    candidates = defaultdict(list)
    for index, draw in frame['draws'].items():
        draw['matrices'] = {name: frame['matrices'][index, name] for name in MATRIX_NAMES
                            if (index, name) in frame['matrices']}
        if draw['cpu_snapshot'] != (len(draw['matrices']) == len(MATRIX_NAMES)):
            frame['issues'].append(f'Draw {index} has incomplete or unexpected matrix lines.')
        if not draw['cpu_snapshot'] or len(draw['matrices']) != len(MATRIX_NAMES) or not draw['layout_verified']:
            continue
        draw['current'] = projection(draw['matrices']['current_wvp']['values'], draw['viewport'])
        draw['stock_previous'] = projection(draw['matrices']['stock_previous_wvp']['values'], draw['viewport'])
        if not draw['current'] or not draw['stock_previous']:
            continue
        draw['current_vs_stock_previous'] = compare_projection(draw['current'], draw['stock_previous'])
        if (draw['vs'] not in RIGID_SHADERS or draw['indirect'] or draw['ib'] == '0x0' or
                (draw['rt4'] == '0x0' and draw.get('rt4_view', '0x0') == '0x0')):
            continue
        signature = tuple(draw[field] for field in GEOMETRY_FIELDS)
        # Identical transforms can be recorded more than once for the same
        # indexed geometry. Divergent transforms remain separate candidates.
        duplicate = next((peer for peer in candidates[signature] if all(
            draw['matrices'][name]['values'] == peer['matrices'][name]['values'] for name in MATRIX_NAMES
        )), None)
        if duplicate is None:
            draw['duplicate_draws'] = []
            candidates[signature].append(draw)
        else:
            duplicate['duplicate_draws'].append(index)
            draw['duplicate_of'] = duplicate['draw']

    if any(index not in frame['draws'] for index, _ in frame['matrices']):
        frame['issues'].append('Matrix lines refer to a draw missing from this dispatch.')
    del frame['matrices']
    frame['draws'] = list(frame['draws'].values())
    return candidates


def pair_provenance(prior, current, metadata_required):
    if prior['session'] != current['session']:
        return 'different_trace_sessions'
    if prior['dispatch'] + 1 != current['dispatch']:
        return 'dispatch_ids_are_not_consecutive'
    if prior['issues'] or current['issues']:
        return 'incomplete_trace_records'
    a, b = prior['capture'], current['capture']
    if a is None or b is None:
        return 'capture_metadata_missing' if metadata_required else 'trace_only'
    if any(a[key] != b[key] for key in ('burst', 'context_id', 'render_width', 'render_height',
                                      'upscale_width', 'upscale_height')):
        return 'capture_context_or_extent_changed'
    if any(a[key] + 1 != b[key] for key in ('sequence', 'recorded_present_interval', 'submitted_present_interval')):
        return 'capture_intervals_are_not_consecutive'
    if any(c['reset'] or c['submissions'] != 1 or not c['source_same_dispatch'] or not c['recording_replaced']
           for c in (a, b)):
        return 'capture_history_or_submission_uncertain'
    return 'consecutive_capture_metadata'


def match_draws(previous, current, origin_tolerance_px, depth_tolerance):
    proposed, reverse = [], Counter()
    for signature, draws in current.items():
        eligible = previous.get(signature, ())
        for draw in draws:
            options = []
            for prior in eligible:
                comparison = compare_projection(prior['current'], draw['stock_previous'])
                if (comparison['origin_distance_px'] <= origin_tolerance_px and
                        comparison['clip_w_relative_difference'] <= depth_tolerance):
                    options.append((prior, comparison))
            proposed.append((draw, options))
            reverse.update(prior['draw'] for prior, _ in options)

    for draw, options in proposed:
        match = {'candidate_prior_draws': [prior['draw'] for prior, _ in options]}
        if not options:
            match['status'] = 'no_prior_geometry_and_origin_match'
        elif len(options) > 1 or reverse[options[0][0]['draw']] > 1:
            match['status'] = 'ambiguous'
        else:
            prior, comparison = options[0]
            match.update(
                status='unique_candidate', prior_draw=prior['draw'],
                prior_current_vs_stock_previous=comparison,
                prior_current_vs_current=compare_projection(prior['current'], draw['current']),
                prior_world_vs_current_world=compare_axes(world_axes(prior), world_axes(draw)),
                prior_world_origin_distance=math.dist(prior['matrices']['world']['values'][12:15],
                                                      draw['matrices']['world']['values'][12:15]),
            )
        draw['prior_match'] = match


def analyze(log, metadata_dir=None, origin_tolerance_px=2.0, depth_tolerance=0.02):
    frames, issues, line_count, digest = parse_trace(log)
    captures, metadata_issues = load_captures(metadata_dir)
    repeated_dispatches = {dispatch for dispatch, count in Counter(frame['dispatch'] for frame in frames).items()
                           if count > 1}
    previous, previous_candidates = None, None
    for current in frames:
        metadata_available = captures
        if current['dispatch'] in repeated_dispatches and current['dispatch'] in captures:
            metadata_available = {}
            current['issues'].append('This dispatch ID exists in more than one trace session.')
        candidates = prepare_frame(current, metadata_available)
        status = 'no_earlier_trace_dispatch' if previous is None else pair_provenance(previous, current, metadata_dir is not None)
        current['prior_pair_provenance'] = status
        if status in ('consecutive_capture_metadata', 'trace_only'):
            match_draws(previous_candidates, candidates, origin_tolerance_px, depth_tolerance)
            current['prior_candidates_truncated'] = any(
                frame['summary']['omitted'] != 0 or not frame['summary']['descriptor_bounds_ok'] or
                frame['summary'].get('descriptor_evictions', 0) != 0
                for frame in (previous, current)
            )
        current['matches'] = dict(Counter(draw['prior_match']['status'] for draw in current['draws'] if 'prior_match' in draw))
        previous, previous_candidates = current, candidates
    return {
        'source': {'log': log.name, 'sha256': digest, 'trace_lines': line_count,
                   'metadata_directory': str(metadata_dir) if metadata_dir else None},
        'criteria': {'origin_tolerance_px': origin_tolerance_px, 'relative_clip_w_tolerance': depth_tolerance,
                     'geometry_fields': GEOMETRY_FIELDS},
        'interpretation': ('Matrices are CPU snapshots taken during draw recording. A geometry and origin match is '
                           'an identity candidate among observed draws. Projected roll is a fit in the local tire plane and can include '
                           'changes in camera, projection, or steering. Compare the two previous-pose candidates '
                           'and inspect the captured spokes. These observations cannot establish GPU-executed '
                           'matrix values or prove that a wheel surface reached the FSR input.'),
        'issues': issues + metadata_issues,
        'dispatches': frames,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='ReShade log or the extracted NativeWheelTrace lines')
    parser.add_argument('--metadata-dir', type=Path, help='Directory containing matching wheel capture JSON files')
    parser.add_argument('--origin-tolerance-px', type=float, default=2.0)
    parser.add_argument('--relative-clip-w-tolerance', type=float, default=0.02)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.metadata_dir is not None and not args.metadata_dir.is_dir():
        parser.error('metadata directory does not exist')
    if not 0 < args.origin_tolerance_px <= 100 or not 0 < args.relative_clip_w_tolerance <= 1:
        parser.error('matching tolerances must be finite and positive')
    try:
        result = analyze(args.log, args.metadata_dir, args.origin_tolerance_px, args.relative_clip_w_tolerance)
        payload = json.dumps(result, indent=2, allow_nan=False) + '\n'
        if args.output:
            args.output.write_text(payload, encoding='utf-8')
        else:
            print(payload, end='')
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
