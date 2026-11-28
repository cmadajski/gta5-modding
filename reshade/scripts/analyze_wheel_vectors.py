#!/usr/bin/env python3
"""Measure FSR input motion vectors and compare wheel motion with a chassis patch."""

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, PngImagePlugin

from analyze_prehud_captures import FORMATS as SCENE_FORMATS
from analyze_prehud_captures import decode_pixels, read_pixels, write_preview


MOTION_FORMATS = {
    2: ('R32G32B32A32_FLOAT', '<f4', 4),
    10: ('R16G16B16A16_FLOAT', '<f2', 4),
    16: ('R32G32_FLOAT', '<f4', 2),
    34: ('R16G16_FLOAT', '<f2', 2),
    37: ('R16G16_SNORM', '<i2', 2),
}
EXTENT_KEYS = (
    'width', 'height', 'motion_width', 'motion_height',
    'render_width', 'render_height', 'upscale_width', 'upscale_height',
)
ID_KEYS = ('sequence', 'dispatch_id', 'context_id')
PRESENT_KEYS = ('recorded_present_interval', 'submitted_present_interval', 'captured_tick_ms')
BURST_NAME = re.compile(r'(NativeWheel-\d+-\d+-\d+)-\d+\.json')
FLAG_KEYS = (
    'source_same_dispatch', 'recording_replaced', 'display_resolution_motion_vectors',
    'jitter_cancellation', 'reset',
)
SCALE_KEYS = ('scale_x', 'scale_y', 'jitter_x', 'jitter_y', 'frame_time_ms')
HUE_NOTE = (
    'FSR vector direction in image coordinates (x right, y down). Red points right, '
    'yellow-green down, cyan left, and violet up. Brightness is min(1, gain multiplied by '
    'vector length in output pixels). Black is zero and magenta is an invalid vector.'
)
INTERPRETATION = (
    'Inspect the before-scene previews for the same visibly rotating spokes, a stable camera, '
    'an unobstructed wheel and a rigid nearby chassis patch. A flat residual by itself cannot '
    'establish whether motion vectors encode wheel rotation. The fitted coefficient describes '
    'projected circulation in the stored vector convention. Wheel pose, perspective, and '
    'the frame-to-frame vector convention matter when interpreting physical rotation.'
)


def decode_motion(path, width, height, dxgi_format):
    """Decode the first two resource channels as sampled by the native shader."""
    _, dtype, channels = MOTION_FORMATS[dxgi_format]
    encoded = read_pixels(path, width, height, np.dtype(dtype).itemsize * channels)
    vectors = encoded.view(dtype)[..., :2].astype(np.float32)
    if dxgi_format == 37:
        vectors /= 32767
        np.maximum(vectors, -1, out=vectors)
    return vectors


def load_metadata(path):
    metadata = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(metadata, dict):
        raise ValueError('capture metadata must be an object')
    for key in (*EXTENT_KEYS, *ID_KEYS, 'dxgi_format', 'motion_dxgi_format', 'submissions'):
        value = metadata[key]
        if type(value) is not int or value < (0 if key == 'submissions' else 1):
            raise ValueError(f'invalid integer metadata field {key}')
    for key in FLAG_KEYS:
        if type(metadata[key]) is not bool:
            raise ValueError(f'{key} must be a JSON boolean')
    for key in PRESENT_KEYS:
        if key in metadata and (type(metadata[key]) is not int or metadata[key] < 0):
            raise ValueError(f'invalid integer metadata field {key}')
    for key in SCALE_KEYS:
        value = metadata[key]
        if key == 'frame_time_ms' and value is None:
            continue  # The capture exporter represents nonfinite dispatch timing as null.
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError(f'{key} must be finite')
    if metadata['dxgi_format'] not in SCENE_FORMATS:
        raise ValueError(f"unsupported scene DXGI_FORMAT {metadata['dxgi_format']}")
    if metadata['motion_dxgi_format'] not in MOTION_FORMATS:
        raise ValueError(f"unsupported motion DXGI_FORMAT {metadata['motion_dxgi_format']}")
    if (metadata['width'], metadata['height']) != (metadata['upscale_width'], metadata['upscale_height']):
        raise ValueError('scene size must equal the FSR upscale extent')
    active = 'upscale' if metadata['display_resolution_motion_vectors'] else 'render'
    active_size = (metadata[f'{active}_width'], metadata[f'{active}_height'])
    if active_size[0] > metadata['motion_width'] or active_size[1] > metadata['motion_height']:
        raise ValueError('active FSR motion viewport exceeds the captured allocation')
    return metadata, active_size


def metadata_issues(metadata):
    issues = []
    if not metadata['source_same_dispatch']:
        issues.append('Motion and scene provenance does not establish a shared FSR dispatch.')
    if metadata['submissions'] == 0:
        issues.append('No submission was observed for this recording.')
    if metadata['submissions'] > 1:
        issues.append('The graphics command recording was replayed.')
    if not metadata['recording_replaced']:
        issues.append('The graphics command recording could still be replayed at export.')
    if metadata['reset']:
        issues.append('FSR temporal history was reset.')
    if metadata['jitter_cancellation']:
        issues.append('FSR jitter cancellation is enabled. The archived vectors have no jitter correction applied.')
    if metadata['frame_time_ms'] is None or not 0 < metadata['frame_time_ms'] <= 250:
        issues.append('Frame time is outside the native blur validity range (0, 250] ms.')
    if metadata['scale_x'] == 0 or metadata['scale_y'] == 0:
        issues.append('A zero FSR scale component makes one output displacement axis unobservable.')
    return issues


def scalar_stats(values):
    if not len(values):
        return None
    minimum, median, p95, maximum = np.quantile(values, (0, 0.5, 0.95, 1))
    return {
        'min': float(minimum), 'median': float(median), 'p95': float(p95),
        'max': float(maximum), 'mean': float(np.mean(values, dtype=np.float64)),
    }


def vector_stats(vectors):
    if not len(vectors):
        return None
    vectors = vectors.astype(np.float64, copy=False)
    return {
        'mean_xy': np.mean(vectors, axis=0).tolist(),
        'median_xy': np.median(vectors, axis=0).tolist(),
        'magnitude': scalar_stats(np.hypot(vectors[:, 0], vectors[:, 1])),
        'nonzero': int(np.count_nonzero(np.any(vectors != 0, axis=1))),
    }


def circulation_fit(residual, tangent):
    """Fit a constant 2D offset and one elliptical tangent field by least squares."""
    if len(residual) < 3:
        return None
    mean_residual, mean_tangent = residual.mean(axis=0), tangent.mean(axis=0)
    centered_residual = residual - mean_residual
    centered_tangent = tangent - mean_tangent
    denominator = np.sum(centered_tangent ** 2)
    if denominator == 0:
        return None
    coefficient = float(np.sum(centered_residual * centered_tangent) / denominator)
    translation_error = float(np.sum(centered_residual ** 2))
    fit_error = float(np.sum((centered_residual - coefficient * centered_tangent) ** 2))
    return {
        'coefficient': coefficient,
        'constant_offset_xy_output_pixels': (mean_residual - coefficient * mean_tangent).tolist(),
        'translation_only_rmse_output_pixels': math.sqrt(translation_error / len(residual)),
        'circulation_rmse_output_pixels': math.sqrt(fit_error / len(residual)),
        'fraction_of_translation_only_squared_error_explained': (
            1 - fit_error / translation_error if translation_error else None
        ),
    }


def sample_regions(raw, scaled, valid, centers_x, centers_y, wheel, reference, annulus):
    """Use native motion texels whose mapped centers fall within each output-pixel ROI."""
    report, arrays = {}, {}
    baseline = None
    if reference is not None:
        x, y, width, height = reference
        rectangle = ((centers_x >= x) & (centers_x < x + width))[None, :] & (
            (centers_y >= y) & (centers_y < y + height)
        )[:, None]
        reference_vectors = scaled[rectangle & valid]
        spread = None
        if len(reference_vectors):
            baseline = np.median(reference_vectors, axis=0)
            spread = scalar_stats(np.linalg.norm(reference_vectors - baseline, axis=1))
        report['reference'] = {
            'xywh_output_pixels': reference,
            'total_texels': int(np.count_nonzero(rectangle)),
            'valid_texels': len(reference_vectors),
            'raw_rg': vector_stats(raw[rectangle & valid]),
            'output_pixel_xy': vector_stats(reference_vectors),
            'deviation_from_median_output_pixels': spread,
        }
    if wheel is None:
        return report, arrays, baseline

    cx, cy, rx, ry = wheel
    nx = (centers_x - cx) / rx
    ny = (centers_y - cy) / ry
    radius = np.hypot(nx[None, :], ny[:, None])
    ellipse = radius <= 1
    report['wheel'] = {
        'center_radii_output_pixels': wheel,
        'total_texels': int(np.count_nonzero(ellipse)),
        'valid_texels': int(np.count_nonzero(ellipse & valid)),
        'raw_rg': vector_stats(raw[ellipse & valid]),
        'output_pixel_xy': vector_stats(scaled[ellipse & valid]),
    }
    if baseline is None:
        return report, arrays, baseline

    ring = (radius >= annulus[0]) & (radius <= annulus[1]) & (radius > 0)
    rows, cols = np.nonzero(ring & valid)
    positions = np.column_stack((centers_x[cols], centers_y[rows]))
    residual = scaled[rows, cols] - baseline
    normal = np.column_stack((nx[cols] / rx, ny[rows] / ry))
    tangent = np.column_stack((-rx * ny[rows], ry * nx[cols]))
    radial_component = np.sum(residual * normal, axis=1) / np.linalg.norm(normal, axis=1)
    tangential_component = np.sum(residual * tangent, axis=1) / np.linalg.norm(tangent, axis=1)

    # Center the eight 45-degree sectors on the cardinal and diagonal directions.
    sector_width = np.pi / 4
    angle = np.arctan2(ny[rows], nx[cols])
    sector_indices = np.floor((angle + sector_width / 2) / sector_width).astype(int) % 8
    sectors = []
    for i in range(8):
        mask = sector_indices == i
        count = int(np.count_nonzero(mask))
        sectors.append({
            'center_degrees_clockwise_from_right': 45 * i,
            'valid_texels': count,
            'mean_residual_xy_output_pixels': residual[mask].mean(axis=0).tolist() if count else None,
            'mean_radial_output_pixels': float(radial_component[mask].mean()) if count else None,
            'mean_tangential_output_pixels': float(tangential_component[mask].mean()) if count else None,
        })
    report['wheel']['annulus'] = {
        'normalized_radius_range': annulus,
        'total_texels': int(np.count_nonzero(ring)),
        'valid_texels': len(residual),
        'chassis_median_xy_output_pixels': baseline.tolist(),
        'residual_output_pixel_xy': vector_stats(residual),
        'radial_output_pixels': scalar_stats(radial_component),
        'tangential_output_pixels': scalar_stats(tangential_component),
        'angular_sectors': sectors,
        'circulation_fit': circulation_fit(residual, tangent),
    }
    arrays.update({
        'wheel_annulus_centers_output_pixels': positions,
        'wheel_annulus_residual_output_pixels': residual,
        'wheel_annulus_radial_output_pixels': radial_component,
        'wheel_annulus_tangential_output_pixels': tangential_component,
    })
    return report, arrays, baseline


def motion_preview(vectors, valid, output_size, gain):
    # Hue is circular, with angle zero mapped to red and positive y pointing down.
    x = np.where(valid, vectors[..., 0], 0)
    y = np.where(valid, vectors[..., 1], 0)
    hue = np.remainder(np.arctan2(y, x) / (2 * np.pi), 1)
    value = np.minimum(np.hypot(x, y), 1 / gain) * gain
    hsv = np.stack((hue, np.ones_like(hue), value), axis=-1)
    hsv_bytes = np.rint(255 * hsv).astype(np.uint8).tobytes()
    rgb = np.array(Image.frombytes('HSV', (x.shape[1], x.shape[0]), hsv_bytes).convert('RGB'))
    rgb[~valid] = (255, 0, 255)
    # Output texel centers map into the active viewport exactly as in ReadMotionUV.
    width, height = output_size
    rows = ((2 * np.arange(height) + 1) * len(rgb)) // (2 * height)
    cols = ((2 * np.arange(width) + 1) * rgb.shape[1]) // (2 * width)
    return Image.fromarray(rgb[rows[:, None], cols[None, :]])


def draw_arrow(draw, start, vector, magnification):
    vector_length = float(np.linalg.norm(vector))
    # Cap long arrows to keep the crop readable at high gains.
    length = min(64, vector_length * magnification)
    if length < 2:
        return
    direction = vector / vector_length
    end = start + length * direction
    head = min(7, length / 2)
    tail = end - direction * head
    wing = np.array([-direction[1], direction[0]]) * (head / 2)
    points = [tuple(end), tuple(tail + wing), tuple(tail - wing)]
    draw.line((tuple(start), tuple(end)), fill='#111111', width=4)
    draw.line((tuple(start), tuple(end)), fill='#f6ff6b', width=2)
    draw.polygon(points, fill='#f6ff6b', outline='#111111')


def write_wheel_preview(path, scene_path, hue, vectors, valid, centers, wheel, baseline, arrow_gain):
    cx, cy, rx, ry = wheel
    pad = max(4, min(rx, ry) * 0.15)
    left = max(0, math.floor(cx - rx - pad))
    top = max(0, math.floor(cy - ry - pad))
    right = min(hue.width, math.ceil(cx + rx + pad))
    bottom = min(hue.height, math.ceil(cy + ry + pad))
    box = left, top, right, bottom
    width, height = right - left, bottom - top
    zoom = min(8, 360 / max(width, height))
    size = max(1, round(width * zoom)), max(1, round(height * zoom))
    scale = np.array([size[0] / width, size[1] / height])
    with Image.open(scene_path) as image:
        scene = image.crop(box).resize(size, Image.Resampling.BICUBIC)
    motion = hue.crop(box).resize(size, Image.Resampling.NEAREST)
    labels = ('Scene before', 'Motion hue', f'{"Residual" if baseline is not None else "Motion"} arrows x{arrow_gain:g}')
    # Keep label text legible even for very thin projected wheels.
    panel_width = max(size[0], 150)
    preview = Image.new('RGB', (panel_width * 3 + 4, size[1] + 22), '#1b1b1b')
    origin = np.array([left, top])
    ellipse_box = tuple((np.array([cx - rx, cy - ry, cx + rx, cy + ry]) - np.tile(origin, 2)) * np.tile(scale, 2))
    ax = scene.copy()
    for panel in (scene, motion, ax):
        ImageDraw.Draw(panel).ellipse(ellipse_box, outline='#ffffff', width=1)

    draw = ImageDraw.Draw(ax)
    centers_x, centers_y = centers
    cols = np.flatnonzero((centers_x >= cx - rx) & (centers_x <= cx + rx))
    rows = np.flatnonzero((centers_y >= cy - ry) & (centers_y <= cy + ry))
    stride = max(1, math.ceil(max(len(cols), len(rows)) / 15))
    mask = np.hypot((centers_x[cols] - cx)[None, :] / rx, (centers_y[rows] - cy)[:, None] / ry) <= 1
    mask &= valid[np.ix_(rows, cols)]
    row_index, col_index = np.nonzero(mask[::stride, ::stride])
    for row, col in zip(rows[row_index * stride], cols[col_index * stride]):
        start = (np.array([centers_x[col], centers_y[row]]) - origin) * scale
        vector = vectors[row, col] if baseline is None else vectors[row, col] - baseline
        draw_arrow(draw, start, vector * scale, arrow_gain)

    draw = ImageDraw.Draw(preview)
    for i, (panel, label) in enumerate(zip((scene, motion, ax), labels)):
        offset = i * (panel_width + 2)
        draw.text((offset + 3, 4), label, fill='white')
        preview.paste(panel, (offset + (panel_width - size[0]) // 2, 22))
    annotation = PngImagePlugin.PngInfo()
    annotation.add_text('Description', f'{HUE_NOTE} Arrows use the indicated scale and a 64-image-pixel length cap.')
    preview.save(path, pnginfo=annotation)


def analyze_capture(path, destination, output_stem, wheel, reference, annulus, gain, arrow_gain):
    metadata, active_size = load_metadata(path)
    output_size = metadata['width'], metadata['height']
    width, height = output_size
    active_width, active_height = active_size
    if wheel is not None and not (0 <= wheel[0] < width and 0 <= wheel[1] < height):
        raise ValueError('wheel center must be inside the scene preview')
    if reference is not None:
        x, y, patch_width, patch_height = reference
        if not (0 <= x < x + patch_width <= width and 0 <= y < y + patch_height <= height):
            raise ValueError('reference rectangle must be inside the scene preview')

    dxgi_format = metadata['motion_dxgi_format']
    raw = decode_motion(
        path.with_name(f'{path.stem}-Motion.bin'), metadata['motion_width'], metadata['motion_height'], dxgi_format
    )
    active_raw = raw[:active_height, :active_width]
    raw_to_output_pixels = np.array([metadata['scale_x'], metadata['scale_y']]) / active_size * output_size
    if not np.all(np.isfinite(raw_to_output_pixels)):
        raise ValueError('invalid derived FSR scale')
    with np.errstate(over='ignore', invalid='ignore'):
        scaled = active_raw * raw_to_output_pixels
    finite = np.all(np.isfinite(active_raw) & np.isfinite(scaled), axis=-1)
    valid = finite & np.all(np.abs(scaled) <= 4 * np.array(output_size), axis=-1)
    centers_x = (np.arange(active_width) + 0.5) * (width / active_width)
    centers_y = (np.arange(active_height) + 0.5) * (height / active_height)
    regions, region_arrays, baseline = sample_regions(
        active_raw, scaled, valid, centers_x, centers_y, wheel, reference, annulus
    )
    artifacts = {}
    scene_format = metadata['dxgi_format']
    _, bytes_per_pixel = SCENE_FORMATS[scene_format]
    for side in ('Before', 'After'):
        scene_path = path.with_name(f'{path.stem}-Scene{side}.bin')
        encoded = read_pixels(scene_path, width, height, bytes_per_pixel)
        scene, _ = decode_pixels(encoded, scene_format)
        name = f'{output_stem}-WheelScene{side}-preview.png'
        write_preview(destination / name, scene)
        artifacts[f'scene_{side.lower()}'] = name

    hue = motion_preview(scaled, valid, output_size, gain)
    hue_name = f'{output_stem}-MotionHue.png'
    annotation = PngImagePlugin.PngInfo()
    annotation.add_text('Description', f'{HUE_NOTE} Gain is {gain:g} per output pixel.')
    hue.save(destination / hue_name, pnginfo=annotation)
    artifacts['motion_hue'] = hue_name
    if wheel is not None:
        wheel_name = f'{output_stem}-WheelQuiver.png'
        write_wheel_preview(
            destination / wheel_name, destination / artifacts['scene_before'], hue, scaled, valid,
            (centers_x, centers_y), wheel, baseline, arrow_gain,
        )
        artifacts['wheel_quiver'] = wheel_name

    vectors_name = f'{output_stem}-WheelVectors.npz'
    np.savez_compressed(
        destination / vectors_name,
        raw_rg=raw, output_pixel_xy=scaled, valid=valid, raw_to_output_pixels_xy=raw_to_output_pixels,
        centers_x_output_pixels=centers_x, centers_y_output_pixels=centers_y, **region_arrays,
    )
    artifacts['numeric_vectors'] = vectors_name
    raw_finite = np.all(np.isfinite(active_raw), axis=-1)
    return {
        'capture': str(path),
        'metadata': metadata,
        'metadata_issues': metadata_issues(metadata),
        'motion_format': MOTION_FORMATS[dxgi_format][0],
        'active_motion_extent_xy': active_size,
        'raw_to_output_pixels_xy': raw_to_output_pixels.tolist(),
        'active_motion': {
            'total_texels': valid.size,
            'nonfinite_raw_texels': int(np.count_nonzero(~raw_finite)),
            'nonfinite_after_scaling_texels': int(np.count_nonzero(raw_finite & ~finite)),
            'finite_outside_shader_uv_limit_texels': int(np.count_nonzero(finite & ~valid)),
            'valid_texels': int(np.count_nonzero(valid)),
            'nonfinite_padding_texels': (
                int(np.count_nonzero(~np.all(np.isfinite(raw[active_height:]), axis=-1)))
                + int(np.count_nonzero(~np.all(np.isfinite(raw[:active_height, active_width:]), axis=-1)))
            ),
            'finite_raw_rg': vector_stats(active_raw[raw_finite]),
            'valid_output_pixel_xy': vector_stats(scaled[valid]),
        },
        'regions': regions,
        'artifacts': artifacts,
    }


def compare_sequences(captures):
    comparisons = []
    runs = defaultdict(list)
    for capture in captures:
        source = Path(capture['capture'])
        burst = BURST_NAME.fullmatch(source.name)
        runs[source.parent, capture['metadata']['context_id'], burst[1] if burst else None].append(capture)
    for run in runs.values():
        for key in ('dispatch_id', 'sequence', 'recorded_present_interval', 'submitted_present_interval'):
            identity_counts = Counter(capture['metadata'][key] for capture in run if key in capture['metadata'])
            for capture in run:
                if identity_counts[capture['metadata'].get(key)] > 1:
                    capture['metadata_issues'].append(f'Duplicate {key} within this capture group.')
        trusted = sorted(
            (capture for capture in run if not capture['metadata_issues']),
            key=lambda capture: capture['metadata']['dispatch_id'],
        )
        for a, b in zip(trusted, trusted[1:]):
            earlier, later = a['metadata'], b['metadata']
            sequence_gap = later['sequence'] - earlier['sequence']
            dispatch_gap = later['dispatch_id'] - earlier['dispatch_id']
            comparison = {
                'from_capture': a['capture'], 'to_capture': b['capture'],
                'sequence_gap': sequence_gap, 'dispatch_gap': dispatch_gap,
                'adjacent_dispatches_and_sequences': sequence_gap == dispatch_gap == 1,
                'frame_time_ms': [earlier['frame_time_ms'], later['frame_time_ms']],
            }
            comparison.update({
                f'{key}_gap': later[key] - earlier[key]
                for key in PRESENT_KEYS if key in earlier and key in later
            })
            if 'submitted_present_interval_gap' in comparison:
                comparison['distinct_submission_intervals'] = comparison['submitted_present_interval_gap'] != 0
                comparison['adjacent_submission_intervals'] = comparison['submitted_present_interval_gap'] == 1
            matching_keys = ('width', 'height', 'render_width', 'render_height', 'display_resolution_motion_vectors')
            if sequence_gap <= 0:
                comparison['measurement_comparison_unavailable'] = 'Sequence and dispatch ordering disagree.'
            elif comparison.get('submitted_present_interval_gap', 1) <= 0:
                comparison['measurement_comparison_unavailable'] = 'Presentation and dispatch ordering disagree.'
            elif any(earlier[key] != later[key] for key in matching_keys):
                comparison['measurement_comparison_unavailable'] = 'FSR viewport or motion-resolution mode changed.'
            else:
                a_fit, b_fit = (
                    capture['regions'].get('wheel', {}).get('annulus', {}).get('circulation_fit')
                    for capture in (a, b)
                )
                if a_fit is not None and b_fit is not None:
                    comparison['circulation_coefficients'] = [a_fit['coefficient'], b_fit['coefficient']]
                    comparison['circulation_coefficient_delta'] = b_fit['coefficient'] - a_fit['coefficient']
            comparisons.append(comparison)
    return comparisons


def capture_paths(inputs):
    paths = []
    for source in inputs:
        if source.is_dir():
            matches = sorted(
                path for path in source.glob('*.json')
                if BURST_NAME.fullmatch(path.name)
                or path.with_name(f'{path.stem}-Motion.bin').is_file()
            )
            if not matches:
                raise ValueError(f'no capture metadata found in {source}')
            paths.extend(path.resolve() for path in matches)
        else:
            if source.suffix.lower() != '.json' or not source.is_file():
                raise ValueError(f'capture metadata file does not exist at {source}')
            paths.append(source.resolve())
    return list(dict.fromkeys(paths))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs='+', help='Capture metadata JSON files or directories')
    parser.add_argument('--wheel', type=float, nargs=4, action='append', metavar=('CX', 'CY', 'RX', 'RY'),
                        help='Wheel ellipse in scene output pixels. Supply once or once per capture in input order.')
    parser.add_argument('--reference', type=float, nargs=4, action='append', metavar=('X', 'Y', 'W', 'H'),
                        help='Rigid chassis rectangle in scene output pixels. Supply once or once per capture.')
    parser.add_argument('--annulus', type=float, nargs=2, default=(0.25, 0.9), metavar=('INNER', 'OUTER'),
                        help='Normalized radii for wheel circulation measurements (default 0.25 to 0.9)')
    parser.add_argument('--gain', type=float, default=0.125, help='Motion hue brightness per output pixel (default 0.125)')
    parser.add_argument('--arrow-gain', type=float, default=4, help='Wheel arrow magnification (default 4)')
    parser.add_argument('--output-dir', type=Path, help='Destination for reports and previews (default first capture directory)')
    args = parser.parse_args()

    try:
        paths = capture_paths(args.inputs)
        if not 0 <= args.annulus[0] < args.annulus[1] <= 1:
            raise ValueError('--annulus requires 0 <= INNER < OUTER <= 1')
        if not all(math.isfinite(value) and value > 0 for value in (args.gain, args.arrow_gain)):
            raise ValueError('preview gains must be finite and positive')
        for options, name in ((args.wheel, '--wheel'), (args.reference, '--reference')):
            if options is None:
                continue
            if len(options) not in (1, len(paths)):
                raise ValueError(f'supply {name} once or once per capture')
            if any(not all(math.isfinite(value) for value in roi) or min(roi[2:]) <= 0 for roi in options):
                raise ValueError(f'{name} requires finite coordinates and positive dimensions')
        destination = args.output_dir or paths[0].parent
        destination.mkdir(parents=True, exist_ok=True)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    captures, errors = [], []
    duplicate_stems = Counter(path.stem for path in paths)
    for index, path in enumerate(paths):
        try:
            wheel = args.wheel[min(index, len(args.wheel) - 1)] if args.wheel else None
            reference = args.reference[min(index, len(args.reference) - 1)] if args.reference else None
            output_stem = f'{index + 1:02d}-{path.stem}' if duplicate_stems[path.stem] > 1 else path.stem
            captures.append(analyze_capture(path, destination, output_stem, wheel, reference, args.annulus,
                                            args.gain, args.arrow_gain))
        except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
            errors.append({'capture': str(path), 'error': str(error)})
            print(f'{path.name}: {error}')
    report = {
        'coordinates': (
            'Scene output pixels with x right and y down. The origin is the upper left edge. Each native motion '
            'texel center maps to ((x + 0.5) × output width / active motion width, (y + 0.5) × output height / '
            'active motion height). ROI statistics sample these centers directly.'
        ),
        'numeric_export': (
            'NPZ raw_rg is the archived motion rectangle decoded to shader-readable RG, including SNORM '
            'normalization. The original encoded bytes are in Motion.bin. '
            'NPZ output_pixel_xy covers the top-left active viewport and multiplies decoded RG by signed FSR '
            'scale and by output extent divided by active motion extent. The valid mask excludes nonfinite '
            'vectors and components whose magnitude exceeds 4 UV, matching the shader limit.'
        ),
        'wheel_measurement': (
            'The chassis baseline is the componentwise median of valid reference vectors. Wheel annulus '
            'residuals subtract that baseline. Radial is normal to the ellipse and positive outward. '
            'Tangential is along the ellipse and positive clockwise in image coordinates. The least-squares '
            'fit uses an elliptical tangent field proportional to normalized radius and a free constant offset.'
        ),
        'comparison_scope': (
            'Comparisons use successive supplied captures with usable metadata within each named wheel burst, '
            'capture directory and FSR context. Supply a separate wheel and chassis ROI when they move.'
        ),
        'interpretation': INTERPRETATION if args.wheel else 'Full-field measurements only. No wheel ROI was supplied.',
        'motion_hue': {'description': HUE_NOTE, 'gain_per_output_pixel': args.gain},
        'wheel_arrow_gain': args.arrow_gain,
        'captures': captures,
        'comparisons': compare_sequences(captures),
        'errors': errors,
    }
    report_path = destination / 'wheel-vector-analysis.json'
    report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(f'Analyzed {len(captures)} captures. Report: {report_path}')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
