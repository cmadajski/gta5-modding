#!/usr/bin/env python3
"""Verify archived GPU wheel blur against same-frame arc and straight references."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from analyze_prehud_captures import FORMATS, decode_pixels, read_pixels
from analyze_wheel_vectors import circulation_fit, decode_motion, load_metadata, metadata_issues, scalar_stats


def displacement(offset, scale):
    return np.sqrt(np.sum((offset * scale) ** 2, axis=1))


def project(local, curr, stock, roll, time):
    sine, cosine = np.sin(roll * time), np.cos(roll * time)
    x, y, z = local.T
    point = np.column_stack((x, cosine*y - sine*z, sine*y + cosine*z, np.ones_like(x)))
    now = point @ curr
    prev = point @ stock
    clip = now + np.asarray(time)[..., None] * (prev - now)
    valid = np.isfinite(clip).all(axis=1) & (clip[:, 3] > 1e-7)
    uv = np.full((len(local), 2), np.nan, np.float32)
    uv[valid] = .5 + np.array([.5, -.5], np.float32) * clip[valid, :2] / clip[valid, 3, None]
    return uv


def unproject(x, y, depth, size, inv):
    uv = (np.column_stack((x, y)).astype(np.float32) + np.float32(.5)) / size
    ndc = np.column_stack((2*uv[:, 0]-1, 1-2*uv[:, 1], depth, np.ones(len(x), np.float32)))
    local = ndc @ inv
    good = np.isfinite(local).all(axis=1) & (local[:, 3] > 1e-7)
    return local[:, :3] / local[:, 3, None], uv, good


def corners(uv, size):
    pixel = uv * size - np.float32(.5)
    base = np.floor(pixel).astype(np.int32)
    frac = pixel - base
    for delta in ((0, 0), (1, 0), (0, 1), (1, 1)):
        xy = base + delta
        w = np.prod(np.where(delta, frac, 1-frac), axis=1)
        yield xy, w


def motion_samples(uv, raw, motion_size, scale):
    xy = np.minimum((uv * motion_size).astype(int), motion_size.astype(int)-1)
    values = raw[xy[:, 1], xy[:, 0]] * scale
    return values, np.isfinite(values).all(axis=1) & (np.abs(values) <= 4).all(axis=1)


def linear_gather(uv, before, raw, size, motion_size, scale, args, exposure):
    values, finite = motion_samples(uv, raw, motion_size, scale)
    output = before[(uv[:, 1]*size[1]).astype(int), (uv[:, 0]*size[0]).astype(int), :3].astype(np.float32)
    px = values * size
    length = np.linalg.norm(px, axis=1)
    radius = np.minimum(length * .5 * exposure, args.radius)
    strength = np.clip((radius-.5)*2, 0, 1)
    valid = finite & (strength > 0)
    v = np.flatnonzero(valid)
    uv, px, length, radius, strength, values = (a[v] for a in (uv, px, length, radius, strength, values))
    rad_uv = values * (radius / length)[:, None]
    tolerance = np.maximum(1, .25 * length) ** 2
    rgb = output[v].copy()
    weight = np.ones(len(v), dtype=np.float32)
    half = max(1, min(8, args.samples // 2))
    for tap in range(-half, half+1):
        if not tap:
            continue
        pos = uv + (tap/half) * rad_uv
        good = np.flatnonzero(((pos >= .5/size) & (pos <= 1-.5/size)).all(axis=1))
        mv, fin = motion_samples(pos[good], raw, motion_size, scale)
        diff = np.sum((mv * size - px[good])**2, axis=1)
        u = np.clip((diff-.25*tolerance[good])/(.75*tolerance[good]), 0, 1)
        agreement = np.where(fin, 1-u*u*(3-2*u), 0)
        ew = agreement * (.5 if abs(tap) == half else 1)
        for xy, w in corners(pos[good], size):
            xy = np.minimum(np.maximum(xy, 0), size.astype(int)-1)
            rgb[good] += (ew*w)[:, None] * before[xy[:, 1], xy[:, 0], :3]
        weight[good] += ew
    output[v] += strength[:, None] * (rgb/weight[:, None]-output[v])
    return output


def arc_gather(uv, center, local, base, current, stock, roll, size, canonical_image, canonical_id, before, args, exposure):
    def offset(t):
        return project(local, current, stock, roll, t) - base

    def cap(side):
        endpoint = side * np.float32(exposure*.5)
        good = displacement(offset(endpoint), size) <= args.radius
        lo, hi = np.zeros(len(uv), np.float32), np.ones(len(uv), np.float32)
        for _ in range(7):
            mid = (lo+hi)*.5
            ok = displacement(offset(endpoint*mid), size) <= args.radius
            lo, hi = np.where(ok, mid, lo), np.where(ok, hi, mid)
        return np.where(good, abs(endpoint), abs(endpoint)*lo)

    half_time = np.minimum(cap(-1), cap(1))
    endpoints = {-1: offset(-half_time), 1: offset(half_time)}
    span = np.maximum(displacement(endpoints[1], size), displacement(endpoints[-1], size))
    strength = np.where(half_time > 0, np.clip((np.minimum(span,args.radius)-.5)*2, 0, 1), 0)
    half = max(1, min(8, args.samples // 2))
    rgb, weight = center.copy(), np.ones(len(uv), np.float32)
    chord_rgb, chord_weight = rgb.copy(), weight.copy()
    max_bow = np.zeros(len(uv),np.float32)

    def accumulate(delta, tap, rgb, weight):
        pos = uv + delta
        good = np.flatnonzero((displacement(delta, size) <= args.radius) &
            ((pos >= .5/size) & (pos <= 1-.5/size)).all(axis=1) & (strength > 0))
        for xy, w in corners(pos[good], size):
            valid = (xy >= 0).all(axis=1) & (xy < size).all(axis=1) & (w > 0)
            i = np.flatnonzero(valid)
            xx, yy = xy[i].T
            wf = w[i] * (canonical_image[yy, xx] == canonical_id) * (.5 if abs(tap) == half else 1)
            rgb[good[i]] += wf[:, None] * before[yy, xx, :3]
            weight[good[i]] += wf

    for tap in range(-half, half+1):
        if not tap:
            continue
        fraction = np.float32(tap / half)
        delta = offset(half_time * fraction)
        chord_delta = abs(fraction) * endpoints[-1 if tap < 0 else 1]
        np.fmax(max_bow,displacement(delta-chord_delta,size),out=max_bow)
        accumulate(delta,tap,rgb,weight)
        accumulate(chord_delta,tap,chord_rgb,chord_weight)
    rgb = center + strength[:, None] * (rgb/weight[:, None]-center)
    chord_rgb = center + strength[:, None] * (chord_rgb/chord_weight[:, None]-center)
    return rgb, weight > 1, half_time, chord_rgb, max_bow


def save_figure(path, before, after, ys, xs, arc, linear, discriminating, title):
    x0, x1 = max(0, xs.min()-12), min(before.shape[1], xs.max()+13)
    y0, y1 = max(0, ys.min()-12), min(before.shape[0], ys.max()+13)
    # Include adjacent bodywork when an oblique tire has a narrow mask.
    extra = max(0, int((0.82*(y1-y0) - (x1-x0))/2))
    x0, x1 = max(0, x0-extra), min(before.shape[1], x1+extra)
    region = np.s_[y0:y1, x0:x1, :3]
    arc_image = after[region].astype(np.float32)
    line_image = after[region].astype(np.float32)
    arc_image[ys-y0, xs-x0] = arc
    line_image[ys-y0, xs-x0] = linear
    tensors = (before[region], line_image, arc_image, after[region])
    def preview(rgb):
        z = np.maximum(rgb.astype(np.float32), 0)
        z = z / (1+z)
        z = np.where(z <= .0031308, 12.92*z, 1.055*z**(1/2.4)-.055)
        return Image.fromarray(np.rint(255*z).astype(np.uint8))
    scale = min(3.0, 240/max(x1-x0, y1-y0))
    w, h = int((x1-x0)*scale), int((y1-y0)*scale)
    panel_w = max(w, 176)
    result = Image.new('RGB', (panel_w*4+50, h+76), '#141a20')
    draw = ImageDraw.Draw(result)
    try:
        header = ImageFont.truetype('/usr/share/fonts/dejavu/DejaVuSans.ttf', 15)
        label = ImageFont.truetype('/usr/share/fonts/dejavu/DejaVuSans.ttf', 13)
    except OSError:
        header = label = ImageFont.load_default()
    draw.text((12, 10), title, fill='white', font=header)
    for i, (name, rgb) in enumerate(zip(('Scene before blur', 'Straight prediction', 'Arc prediction', 'GPU scene after blur'), tensors)):
        x = 12+i*(panel_w+8)
        draw.text((x, 32), name, fill='white', font=label)
        result.paste(preview(rgb).resize((w,h), Image.Resampling.LANCZOS), (x, 51))
    draw.text((12,h+55), f'Predictions replace {len(xs):,} eligible pixels in the GPU crop. {discriminating:,} distinguish the two models.',
        fill='#b7bcc1', font=label)
    result.save(path)


def load_capture_images(path, m):
    w, h, fmt = (m[key] for key in ('width','height','dxgi_format'))
    file_for = lambda kind: path.with_name(f'{path.stem}-{kind}.bin')
    before, after = (decode_pixels(read_pixels(file_for(kind),w,h,FORMATS[fmt][1]),fmt)[0]
                     for kind in ('SceneBefore','SceneAfter'))
    raw = decode_motion(file_for('Motion'),m['motion_width'],m['motion_height'],m['motion_dxgi_format'])
    return before, after, raw


def decode_scene_depth(path, metadata, width, height):
    footprint_format = metadata['scene_depth_footprint_dxgi_format']
    row_bytes = metadata['scene_depth_row_bytes']
    if row_bytes not in (width*4,width*8) or path.stat().st_size != row_bytes*height:
        raise ValueError(f'{path.name} depth plane extent or encoded row mismatch')
    bytes_per_texel = row_bytes//width
    raw = read_pixels(path,width,height,bytes_per_texel)
    if footprint_format in (39,40,41) and bytes_per_texel == 4:
        return raw.view('<f4')[...,0], 'float32'
    if footprint_format in (19,20,21) and bytes_per_texel == 8:
        return raw.view('<f4')[...,0], 'float32 with trailing stencil/padding word'
    raise ValueError(f'{path.name} unsupported depth plane format {footprint_format}, {bytes_per_texel} bytes/texel')


def baseline_linear(path, m, args):
    before, after, raw = load_capture_images(path, m)
    w, h = m['width'],m['height']
    size = np.array((w,h),dtype=np.float32)
    prefix = 'upscale' if m['display_resolution_motion_vectors'] else 'render'
    motion_size = np.array((m[f'{prefix}_width'],m[f'{prefix}_height']),dtype=np.float32)
    scale = np.array([m['scale_x'],m['scale_y']],np.float32)/motion_size
    x0 = w//2 if args.split else 0
    y,x = np.meshgrid(np.arange(3,h,19),np.arange(x0+5,w,17),indexing='ij')
    x,y = x.ravel(),y.ravel()
    uv=(np.column_stack((x,y)).astype(np.float32)+.5)/size
    exposure = min(1,max(0,args.shutter/360 if args.shutter_ms is None else args.shutter_ms/m['frame_time_ms']))
    pred = linear_gather(uv,before,raw,size,motion_size,scale,args,exposure)
    if m['dxgi_format']==10:
        pred = pred.astype(np.float16).astype(np.float32)
    observed, original = after[y,x,:3].astype(np.float32),before[y,x,:3].astype(np.float32)
    finite = (np.isfinite(observed)&np.isfinite(original)&np.isfinite(pred)).all(axis=1)
    original_mae = np.mean(abs(observed-original),axis=1)
    linear_mae = np.mean(abs(observed-pred),axis=1)
    changed = finite & (original_mae >= args.gap)
    return dict(finite_sampled_pixels=int(finite.sum()),visibly_changed_sampled_pixels=int(changed.sum()),
        before_error_changed_subset=scalar_stats(original_mae[changed]),
        linear_error_changed_subset=scalar_stats(linear_mae[changed]))


def analyze(path, args):
    m, active_size = load_metadata(path)
    cov = m.get('wheel_coverage', {})
    entry = dict(capture=path.stem, dispatch=m['dispatch_id'], interval=m['submitted_present_interval'], wheel_coverage=cov,
                 metadata_issues=metadata_issues(m), arc_controls_enabled=m.get('wheel_arcs_enabled'), blur_recorded=m.get('blur_enabled'),
                 auxiliary=m.get('wheel_mask_captured',False))
    if not entry['auxiliary'] or entry['metadata_issues']:
        if args.linear_baseline and entry['blur_recorded'] and not entry['metadata_issues']:
            entry['linear_baseline'] = baseline_linear(path,m,args)
        return entry

    aux_size = m['wheel_width'],m['wheel_height']
    if any(type(value) is not int or value <= 0 or value > 8192 for value in aux_size):
        raise ValueError(f'{path.name} has an invalid wheel mask extent')
    if any(allocated < active for allocated, active in zip(aux_size, active_size)):
        raise ValueError(f'{path.name} wheel mask is smaller than the active render viewport')
    motion_size = np.array((m['render_width'],m['render_height']),dtype=np.float32)
    if m['display_resolution_motion_vectors'] or active_size != (m['render_width'],m['render_height']):
        raise ValueError(f'{path.name} wheel arc capture requires render-resolution motion')
    size = np.array((m['width'],m['height']),dtype=np.float32)
    aw, ah = aux_size
    path_for = lambda kind: path.with_name(f'{path.stem}-{kind}.bin')
    fid = m['recorded_present_interval']
    stride = m['wheel_parameter_stride']
    param_bytes = m['wheel_parameter_bytes']
    if stride != 512 or not 0 < param_bytes <= 65536 * stride or param_bytes % stride != 0:
        raise ValueError(f'{path.name} has an invalid wheel parameter layout')
    if param_bytes != path_for('WheelParameters').stat().st_size:
        raise ValueError(f'{path.name} has an incomplete wheel parameter snapshot')
    params_f = np.memmap(path_for('WheelParameters'),mode='r',dtype='<f4',shape=(param_bytes//stride,stride//4))
    params_u = params_f.view('<u4')
    counts_good = (params_u[:,81] == fid) & (params_u[:,82] != 0) & (params_u[:,83] == 1) & (
        params_u[:,84:86] == motion_size.astype(np.uint32)).all(axis=1) & (params_u[:,86] == np.arange(len(params_u)))
    encoded = read_pixels(path_for('WheelMask'),aw,ah,16).view('<f4')[...,3] * np.float32(65536)
    wheel_z = read_pixels(path_for('WheelDepth'),aw,ah,4).view('<f4')[...,0]
    scene_z, depth_encoding = decode_scene_depth(path_for('SceneDepth'),m,aw,ah)
    shape = int(motion_size[1]),int(motion_size[0])
    encoded, wheel_z, scene_z = (v[:shape[0],:shape[1]] for v in (encoded,wheel_z,scene_z))
    finite_integer = np.isfinite(encoded) & (encoded == np.floor(encoded)) & (encoded >= 1) & (encoded <= min(65536,len(params_u)))
    my,mx = np.nonzero(finite_integer)
    sample_ids = encoded[my,mx].astype(int) - 1
    zy,sz = wheel_z[my,mx], scene_z[my,mx]
    depth_ok = np.isfinite(zy)&np.isfinite(sz)&(zy>=0)&(zy<=1)&(sz>=0)&(sz<=1) & (abs(zy-sz) <= np.maximum(1e-7,2e-7*np.maximum(abs(zy),abs(sz))))
    actual_frames, actual_counts = np.unique(params_u[sample_ids,81],return_counts=True)
    depth_and_identity = depth_ok & counts_good[sample_ids]
    raw = decode_motion(path_for('Motion'),m['motion_width'],m['motion_height'],m['motion_dxgi_format'])
    scale = np.array([m['scale_x'],m['scale_y']],np.float32) / motion_size
    pair_ok = np.zeros(len(sample_ids),dtype=bool)
    endpoint_error = np.full((len(sample_ids),2),np.nan,dtype=np.float32)
    for index in np.unique(sample_ids[depth_and_identity]):
        i = np.flatnonzero(depth_and_identity & (sample_ids == index))
        x,y = mx[i],my[i]
        inverse = params_f[index,64:80].reshape(4,4)
        corrected = params_f[index,32:48].reshape(4,4)
        local, raster_uv, inverse_ok = unproject(x,y,zy[i],motion_size,inverse)
        previous = project(local,corrected,corrected,0.0,0.0)
        expected = (previous-raster_uv)*motion_size
        observed_uv = raw[y,x]*scale
        difference = observed_uv*motion_size-expected
        pair_ok[i] = inverse_ok & np.isfinite(difference).all(axis=1) & (abs(observed_uv)<=4).all(axis=1) & (
            abs(difference) <= .25 + .0005*abs(expected)).all(axis=1)
        endpoint_error[i] = difference
    valid_errors = np.isfinite(endpoint_error).all(axis=1)
    qualified = depth_and_identity & pair_ok
    visible_ids, visible_counts = np.unique(sample_ids[qualified],return_counts=True)
    visible_canonical = params_u[visible_ids,82]
    entry['raster'] = dict(active_wh=motion_size.astype(int).tolist(),mask_wh=list(aux_size),frame_expected=fid,
        masked_texels=len(sample_ids),unique_pool_ids=len(np.unique(sample_ids)),matching_depth_texels=int(depth_ok.sum()),
        scene_depth_encoding=depth_encoding,
        parameter_frames_on_mask={str(int(i)):int(n) for i,n in zip(actual_frames,actual_counts)},
        current_frame_pool_records=int(counts_good.sum()),current_frame_canonical_ids=len(np.unique(params_u[counts_good,82])),
        depth_and_identity_texels=int(depth_and_identity.sum()),
        native_endpoint_error_render_px=scalar_stats(np.linalg.norm(endpoint_error[valid_errors],axis=1)),
        native_endpoint_rejected_texels=int(np.count_nonzero(depth_and_identity & ~pair_ok)),
        qualified_texels=int(qualified.sum()),qualified_pool_ids=len(visible_ids),qualified_canonical_ids=len(np.unique(visible_canonical)),
        finite_abs_depth_error=scalar_stats(abs(zy-sz)[np.isfinite(zy)&np.isfinite(sz)]))
    if not np.any(qualified):
        return entry

    pool_map = np.full(shape, -1, np.int32)
    pool_map[my[qualified],mx[qualified]] = sample_ids[qualified]
    jitter = np.array([m['jitter_x'],m['jitter_y']],np.float32) / motion_size
    source = [np.floor(((np.arange(n,dtype=np.float32)+.5)/n+jitter[i])*motion_size[i]).astype(np.int32)
              for i,n in enumerate(size.astype(int))]
    ax,ay = source
    ax_ok, ay_ok = (ax>=0)&(ax<shape[1]),(ay>=0)&(ay<shape[0])
    mapped = pool_map[ay.clip(0,shape[0]-1)[:,None],ax.clip(0,shape[1]-1)[None,:]]
    mapped[~ay_ok,:] = -1
    mapped[:,~ax_ok] = -1
    active = mapped >= 0
    if args.split:
        active[:, :len(ax)//2] = False
    canonical_image = np.zeros_like(mapped,dtype=np.uint32)
    positive = mapped >= 0
    canonical_image[positive] = params_u[mapped[positive],82]
    entry['raster']['qualified_output_pixels'] = int(active.sum())

    fmt = m['dxgi_format']
    before, after = (decode_pixels(read_pixels(path_for(kind),*size.astype(int),FORMATS[fmt][1]),fmt)[0]
                     for kind in ('SceneBefore','SceneAfter'))
    exposure = min(1,max(0, args.shutter / 360 if args.shutter_ms is None else args.shutter_ms/m['frame_time_ms']))
    entry['oracle_controls']=dict(radius=args.radius,samples=args.samples,exposure_fraction=exposure,split=args.split)
    entry['wheel']=[]
    all_points=[]
    all_y, all_x = np.nonzero(active)
    all_indices = mapped[all_y,all_x]
    index_order = np.argsort(all_indices,kind='stable')
    sorted_indices = all_indices[index_order]
    for index,i_count in zip(visible_ids, visible_counts):
        start, stop = np.searchsorted(sorted_indices,[index,index+1])
        pixels = index_order[start:stop]
        cy,cx = all_y[pixels],all_x[pixels]
        pf = params_f[index]
        mats = [pf[i:i+16].reshape(4,4) for i in (0,16,32,48,64)]
        _,current,corrected,stock,inverse = mats
        roll = float(pf[80])
        identity = int(params_u[index,82])
        record=dict(pool_index=int(index),canonical_id=identity,roll_degrees=float(np.rad2deg(roll)),valid_texels=int(i_count),
                    eligible_output_pixels=len(cx))
        entry['wheel'].append(record)
        if not len(cx) or not 0 < abs(roll) <= 3.141593 or not np.isfinite(roll):
            continue
        x,y = ax[cx],ay[cy]
        uv=(np.column_stack((cx,cy)).astype(np.float32)+.5)/size
        local,raster_uv,good=unproject(x,y,wheel_z[y,x],motion_size,inverse)
        projected=project(local,current,stock,roll,0)
        reprojection=displacement(projected-raster_uv,size)
        good &= np.isfinite(projected).all(axis=1)
        cx,cy,uv,local,projected,raster_uv,reprojection,x,y=(v[good] for v in (cx,cy,uv,local,projected,raster_uv,reprojection,x,y))
        if len(cx)==0:
            continue
        endpoint=project(local,current,stock,roll,1)
        stock_endpoint=project(local,current,stock,0.,1)
        corrected_endpoint=project(local,corrected,corrected,0.,0)
        small=np.float32(.001)
        tangent=(project(local,stock,stock,roll,small)-project(local,stock,stock,roll,-small))*size/(2*small)
        contribution=(endpoint-stock_endpoint)*size
        sampled=raw[y,x]*scale*size
        vdiff=(endpoint-projected)*size
        vm=(np.isfinite(vdiff)&np.isfinite(sampled)).all(axis=1)
        vv=(np.isfinite(tangent)&np.isfinite(contribution)&np.isfinite(stock_endpoint)&np.isfinite(sampled)).all(axis=1)
        record.update(reprojection_error_px=scalar_stats(reprojection),
            corrected_endpoint_closure_px=scalar_stats(displacement(endpoint-corrected_endpoint,size)[np.isfinite(endpoint-corrected_endpoint).all(axis=1)]),
            rotational_full_frame_displacement_px=scalar_stats(np.linalg.norm(contribution[vv],axis=1)),
            rotation_fit_against_camera_removed_observed=circulation_fit((sampled-(stock_endpoint-projected)*size)[vv],tangent[vv]),
            raw_minus_corrected_px=scalar_stats(np.linalg.norm((sampled-vdiff)[vm],axis=1)),
            raw_plus_corrected_px=scalar_stats(np.linalg.norm((sampled+vdiff)[vm],axis=1)))
        center=before[cy,cx,:3].astype(np.float32)
        actual=after[cy,cx,:3].astype(np.float32)
        curved,gathered,half_time,chord,bow = arc_gather(uv,center,local,projected,current,stock,roll,size,canonical_image,identity,before,args,exposure)
        linear = linear_gather(uv,before,raw,size,motion_size,scale,args,exposure)
        if fmt == 10:
            curved,linear = curved.astype(np.float16).astype(np.float32),linear.astype(np.float16).astype(np.float32)
            chord = chord.astype(np.float16).astype(np.float32)
        finite = (np.isfinite(curved)&np.isfinite(linear)&np.isfinite(actual)).all(axis=1)
        curve_error=np.mean(abs(actual-curved),axis=1)
        line_error=np.mean(abs(actual-linear),axis=1)
        gap=np.mean(abs(curved-linear),axis=1)
        distinguish=finite & (gap >= args.gap)
        chord_distinguish=finite & np.isfinite(chord).all(axis=1) & (np.mean(abs(curved-chord),axis=1) >= args.gap)
        chord_error=np.mean(abs(actual-chord),axis=1)
        informative=finite & gathered
        record.update(valid_inverse_output_pixels=len(cx),gathered_outputs=int(gathered.sum()),half_time=scalar_stats(half_time),
            bounding_box_output_xyxy=[int(cx.min()),int(cy.min()),int(cx.max()+1),int(cy.max()+1)],
            oracle_arc_error=scalar_stats(curve_error[informative]),oracle_linear_error=scalar_stats(line_error[informative]),
            models_disagree_outputs=int(distinguish.sum()),
            distinguish_arc_mae=float(curve_error[distinguish].mean()) if distinguish.any() else None,
            distinguish_linear_mae=float(line_error[distinguish].mean()) if distinguish.any() else None,
            distinguish_arc_closer_fraction=float(np.mean(curve_error[distinguish] < line_error[distinguish])) if distinguish.any() else None,
            max_arc_vs_same_gate_chord_deviation_px=scalar_stats(bow[informative]),
            same_gate_chord_disagree_outputs=int(chord_distinguish.sum()),
            distinguish_same_gate_arc_mae=float(curve_error[chord_distinguish].mean()) if chord_distinguish.any() else None,
            distinguish_same_gate_chord_mae=float(chord_error[chord_distinguish].mean()) if chord_distinguish.any() else None,
            distinguish_same_gate_arc_closer_fraction=float(np.mean(curve_error[chord_distinguish] < chord_error[chord_distinguish])) if chord_distinguish.any() else None)
        all_points.append((record,cy,cx,curved,linear,distinguish))
    for identifier in sorted(set(p[0]['canonical_id'] for p in all_points)):
        parts=[p for p in all_points if p[0]['canonical_id']==identifier]
        discrimination=sum(p[0]['models_disagree_outputs'] for p in parts)
        if discrimination<args.figure_min:
            continue
        figure=args.out/f'{path.stem}-wheel-{identifier}-arc.png'
        y,x,arc,line=(np.concatenate([p[i] for p in parts],axis=0) for i in (1,2,3,4))
        save_figure(figure,before,after,y,x,arc,line,discrimination,
                    f'FSR dispatch {m["dispatch_id"]}  |  Wheel {identifier}  |  Same-frame sampling')
    return entry


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('capture',type=Path,help='NativeWheel capture JSON or directory containing the exported JSON and binaries')
    parser.add_argument('--out',type=Path,help='Write the report and figures here, defaulting to the capture directory')
    shutter = parser.add_mutually_exclusive_group()
    shutter.add_argument('--shutter',type=float,default=180,help='Recorded shutter angle in degrees (default 180)')
    shutter.add_argument('--shutter-ms',type=float,help='Recorded fixed exposure in milliseconds')
    parser.add_argument('--radius',type=float,default=12,help='Recorded maximum radius in output pixels (default 12)')
    parser.add_argument('--samples',type=int,default=9,help='Recorded blur sample count (default 9)')
    parser.add_argument('--split',action='store_true',help='The recorded pass used a left-half unblurred reference')
    parser.add_argument('--gap',type=float,default=.002,help='Threshold for a difference in predicted mean linear RGB')
    parser.add_argument('--figure-min',type=int,default=100,help='Minimum distinguishing pixels per wheel to write a figure')
    parser.add_argument('--linear-baseline',action='store_true',help='Evaluate straight sampling on a pixel grid for frames without a wheel snapshot')
    args=parser.parse_args()
    if not 0 < args.radius <= 32 or not 3 <= args.samples <= 17 or not np.isfinite(args.gap) or args.gap <= 0 or args.figure_min < 1:
        parser.error('radius must be in (0, 32], samples in [3, 17], prediction gap positive and figure minimum at least 1')
    if not 0 < args.shutter <= 360 or (args.shutter_ms is not None and not 0 < args.shutter_ms <= 24):
        parser.error('shutter must be in (0, 360] degrees or (0, 24] milliseconds')
    paths=sorted(args.capture.glob('NativeWheel-*.json')) if args.capture.is_dir() else [args.capture]
    if not paths or not all(path.is_file() and path.suffix == '.json' for path in paths):
        parser.error(f'No NativeWheel capture metadata found at {args.capture}')
    args.out = args.out or (args.capture if args.capture.is_dir() else args.capture.parent)
    args.out.mkdir(parents=True,exist_ok=True)
    results=[]
    for path in paths:
        try:
            entry=analyze(path,args)
        except (KeyError,OSError,TypeError,ValueError) as error:
            parser.error(f'{path.name}: {error}')
        results.append(entry)
        raster = entry.get('raster')
        if raster is not None:
            print(f"Dispatch {entry['dispatch']}: {raster['qualified_texels']:,} of {raster['masked_texels']:,} masked texels qualified.")
    report = args.out/'wheel-arc-oracle.json'
    report.write_text(json.dumps(results,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(f'Analyzed {len(results)} captures. Report {report}')

if __name__=='__main__':
    main()
