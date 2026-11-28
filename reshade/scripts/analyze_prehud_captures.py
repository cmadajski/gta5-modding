#!/usr/bin/env python3
"""Measure same-frame FSR scene changes and render illustrative sRGB previews."""

import argparse
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image, PngImagePlugin


# DXGI_FORMAT values and bytes per tightly packed pixel.
FORMATS = {
    2: ('R32G32B32A32_FLOAT', 16),
    10: ('R16G16B16A16_FLOAT', 8),
    24: ('R10G10B10A2_UNORM', 4),
    26: ('R11G11B10_FLOAT', 4),
    28: ('R8G8B8A8_UNORM', 4),
}
PREVIEW_NOTE = (
    'Illustrative preview of the scene buffer. Negative and nonfinite channels map to black. '
    'Nonnegative values use x/(1+x), the sRGB transfer curve, and 8-bit quantization. '
    'The game applies its own tone mapping, display processing, and HUD composition.'
)


def read_pixels(path, width, height, bytes_per_pixel):
    expected = width * height * bytes_per_pixel
    actual = path.stat().st_size
    if actual != expected:
        raise ValueError(f'{path.name} has {actual} bytes, expected {expected}')
    return np.memmap(path, dtype=np.uint8, mode='r', shape=(height, width, bytes_per_pixel))


def decode_pixels(raw, dxgi_format):
    """Return scene color and the stored alpha bits, when present."""
    if dxgi_format == 2:
        return raw.view('<f4'), raw.view('<u4')[..., 3]
    if dxgi_format == 10:
        return raw.view('<f2'), raw.view('<u2')[..., 3]
    if dxgi_format == 28:
        return raw.astype(np.float64) / 255, raw[..., 3]

    packed = raw.view('<u4')[..., 0]
    if dxgi_format == 24:
        red = (packed & 1023) / 1023
        green = ((packed >> 10) & 1023) / 1023
        blue = ((packed >> 20) & 1023) / 1023
        alpha_bits = packed >> 30
        return np.stack((red, green, blue, alpha_bits / 3), axis=-1), alpha_bits

    # Unsigned R11G11B10 uses the same 5-bit exponent and bias as binary16.
    # Left-align each mantissa in binary16 and keep the sign zero, including
    # for subnormals, infinity, and NaN.
    red = ((packed & 2047) << 4).astype('<u2').view('<f2')
    green = (((packed >> 11) & 2047) << 4).astype('<u2').view('<f2')
    blue = ((packed >> 22) << 5).astype('<u2').view('<f2')
    return np.stack((red, green, blue), axis=-1), None


def compare_regions(before, after, raw_before, raw_after, alpha_before, alpha_after):
    before_rgb, after_rgb = before[..., :3], after[..., :3]
    finite_before, finite_after = np.isfinite(before_rgb), np.isfinite(after_rgb)
    valid_pixels = np.all(finite_before & finite_after, axis=-1)
    finite_pixels = int(np.count_nonzero(valid_pixels))
    changed_rgb = np.any(before_rgb != after_rgb, axis=-1) & valid_pixels
    # Use float64 before subtracting so even opposite float32 extrema have a
    # finite difference. Exclude a whole RGB pixel if any channel is nonfinite.
    deltas = np.zeros(before_rgb.shape, dtype=np.float64)
    np.subtract(before_rgb, after_rgb, out=deltas, where=valid_pixels[..., None], dtype=np.float64)
    np.abs(deltas, out=deltas)

    alpha = None
    if alpha_before is not None:
        changed_alpha = int(np.count_nonzero(alpha_before != alpha_after))
        alpha = {
            'bitwise_equal': changed_alpha == 0,
            'changed_pixels': changed_alpha,
            'nonfinite_before': int(np.count_nonzero(~np.isfinite(before[..., 3]))),
            'nonfinite_after': int(np.count_nonzero(~np.isfinite(after[..., 3]))),
        }
    return {
        'pixels': int(valid_pixels.size),
        'finite_rgb_pixels': finite_pixels,
        'changed_linear_rgb_pixels': int(np.count_nonzero(changed_rgb)),
        'changed_encoded_pixels': int(np.count_nonzero(np.any(raw_before != raw_after, axis=-1))),
        'mean_absolute_linear_rgb_delta': float(deltas.sum() / (3 * finite_pixels)) if finite_pixels else None,
        'max_absolute_linear_rgb_delta': float(deltas.max()) if finite_pixels else None,
        'nonfinite_rgb_channels_before': int(np.count_nonzero(~finite_before)),
        'nonfinite_rgb_channels_after': int(np.count_nonzero(~finite_after)),
        'alpha': alpha,
    }


def write_preview(path, scene):
    rgb = scene[..., :3].astype(np.float64)
    np.nan_to_num(rgb, copy=False, nan=0.0, posinf=0.0, neginf=0.0)
    np.maximum(rgb, 0, out=rgb)
    rgb /= 1 + rgb
    srgb = np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - 0.055)
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text('Description', PREVIEW_NOTE)
    Image.fromarray(np.rint(255 * srgb).astype(np.uint8)).save(path, pnginfo=metadata)


def analyze_pair(metadata_path):
    capture = json.loads(metadata_path.read_text(encoding='utf-8'))
    width, height, dxgi_format, sequence = (
        capture[key] for key in ('width', 'height', 'dxgi_format', 'sequence')
    )
    if any(type(value) is not int or value < 1 for value in (width, height, dxgi_format, sequence)):
        raise ValueError('width, height, dxgi_format, and sequence must be positive integers')
    if dxgi_format not in FORMATS:
        raise ValueError(f'unsupported scene DXGI_FORMAT {dxgi_format}')
    format_name, bytes_per_pixel = FORMATS[dxgi_format]
    directory, stem = metadata_path.parent, metadata_path.stem
    before_path = directory / f'{stem}-SceneBefore.bin'
    after_path = directory / f'{stem}-SceneAfter.bin'
    raw_before = read_pixels(before_path, width, height, bytes_per_pixel)
    raw_after = read_pixels(after_path, width, height, bytes_per_pixel)
    before, alpha_before = decode_pixels(raw_before, dxgi_format)
    after, alpha_after = decode_pixels(raw_after, dxgi_format)

    halves = {}
    for name, columns in (('left', slice(0, width // 2)), ('right', slice(width // 2, width))):
        halves[name] = compare_regions(
            before[:, columns], after[:, columns], raw_before[:, columns], raw_after[:, columns],
            alpha_before[:, columns] if alpha_before is not None else None,
            alpha_after[:, columns] if alpha_after is not None else None,
        )

    previews = {}
    for side, scene in (('Before', before), ('After', after)):
        name = f'{stem}-Scene{side}-preview.png'
        write_preview(directory / name, scene)
        previews[side.lower()] = name
    return {
        'capture': stem,
        'sequence': sequence,
        'width': width,
        'height': height,
        'dxgi_format': dxgi_format,
        'format': format_name,
        'scene_previews': previews,
        'composed_screenshots': sorted(p.name for p in directory.glob(f'*{stem}-Final.png')),
        'halves': halves,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path, help='Directory containing the raw captures and metadata')
    args = parser.parse_args()
    metadata_paths = sorted(
        p for p in args.run_directory.glob('NativePreHUD-*.json')
        if re.fullmatch(r'NativePreHUD-\d+\.json', p.name)
    )
    if not metadata_paths:
        parser.error(f'No NativePreHUD capture metadata found in {args.run_directory}')

    captures = []
    for path in metadata_paths:
        try:
            captures.append(analyze_pair(path))
        except (OSError, ValueError, TypeError, KeyError) as error:
            parser.error(f'{path.name}: {error}')
    output = {
        'rgb_comparison': (
            'Exact decoded scene RGB comparisons before preview encoding. The mean includes all three channels '
            'of every pixel with finite RGB in both captures. Counts of encoded changes compare the entire '
            'stored pixel, including alpha where present.'
        ),
        'preview': PREVIEW_NOTE,
        'captures': captures,
    }
    report = args.run_directory / 'prehud-capture-analysis.json'
    report.write_text(json.dumps(output, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(f'Analyzed {len(captures)} scene pairs. Report: {report}')


if __name__ == '__main__':
    main()
