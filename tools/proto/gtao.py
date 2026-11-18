# GTAO prototype (after XeGTAO / Jimenez 2016), view space rebuilt from UE3's HDR-alpha depth.
import numpy as np, sys
sys.path.insert(0, '/home/claude/proto'); from aolab import *
PI = np.pi
DBG = {}
def gtao(depth, fov=85, R=40.0, slices=3, steps=6, smooth_normals=True, power=1.6, near=30, maxd=5000, base=2, qeps=0.0):
    H, W = depth.shape
    tx = np.tan(np.radians(fov / 2)); ty = tx * H / W
    py, px = np.mgrid[0:H, 0:W].astype(np.float32)
    u = (px + 0.5) / W; v = (py + 0.5) / H
    def P(uu, vv, dd):
        return np.stack([(uu * 2 - 1) * tx * dd, (1 - vv * 2) * ty * dd, dd], -1)
    D = sampler(depth)
    d = depth; p = P(u, v, d)
    # normals: pick the smaller depth step on each axis (keeps silhouettes clean)
    # wide baseline: fp16 depth comes in 0.125-0.25 unit steps, 1-pixel differences are mostly quantization
    bx = base / W; by = base / H
    dR = D(u + bx, v); dL = D(u - bx, v); dDn = D(u, v + by); dUp = D(u, v - by)
    pR = P(u + bx, v, dR); pL = P(u - bx, v, dL); pDn = P(u, v + by, dDn); pUp = P(u, v - by, dUp)
    tx_ = np.where((np.abs(dR - d) < np.abs(d - dL))[..., None], pR - p, p - pL)
    ty_ = np.where((np.abs(dDn - d) < np.abs(d - dUp))[..., None], pDn - p, p - pUp)
    n = np.cross(tx_, ty_); n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
    n = np.where((np.sum(n * p, -1) > 0)[..., None], -n, n)   # face the viewer (n.z alone fails on floors)
    if smooth_normals:   # depth-aware 5x5 average: hides low-poly faceting (the game shades with smooth normals)
        acc = np.zeros_like(n); ws = np.zeros_like(d)
        for oy in range(-2, 3):
            for ox in range(-2, 3):
                nn = np.roll(np.roll(n, oy, 0), ox, 1); dd = np.roll(np.roll(d, oy, 0), ox, 1)
                w = np.clip(1 - np.abs(dd - d) / (d * 0.02 + 0.5), 0, 1)
                acc += nn * w[..., None]; ws += w
        n = acc / ws[..., None]; n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
    DBG['n']=n.copy(); DBG['radPx']=radPx if False else None
    viewV = -p / np.linalg.norm(p, axis=-1, keepdims=True)
    # a visible surface faces the camera: bend back normals that quantization turned away
    nv = np.sum(n * viewV, -1, keepdims=True)
    n = np.where(nv < 0.1, n + viewV * (0.1 - nv), n); n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
    # screen radius in pixels for world radius R at depth d
    radPx = R / (2 * tx * d) * W
    radPx = np.clip(radPx, 2, W * 0.08)          # cap footprint; ...
    Reff = radPx / W * 2 * tx * d                # ...and shrink R with it
    falloffRange = 0.615 * Reff; falloffFrom = Reff * (1 - 0.615)
    fMul = -1 / falloffRange; fAdd = falloffFrom / falloffRange + 1
    noise = noise_ign(px, py); noise2 = noise_ign(px + 37, py + 17)
    vis = np.zeros_like(d)
    for s in range(slices):
        phi = (s + noise) / slices * PI
        dx, dy = np.cos(phi), np.sin(phi)                   # screen dir (y down)
        dir3 = np.stack([dx, -dy, np.zeros_like(dx)], -1)   # view space (y up)
        ortho = dir3 - (np.sum(dir3 * viewV, -1, keepdims=True)) * viewV
        axis = np.cross(dir3, viewV)
        projN = n - axis * np.sum(n * axis, -1, keepdims=True)
        projLen = np.linalg.norm(projN, axis=-1) + 1e-9
        sgn = np.sign(np.sum(ortho * projN, -1))
        cosN = np.clip(np.sum(projN * viewV, -1) / projLen, -1, 1)
        nang = sgn * np.arccos(cosN)
        low0 = np.cos(nang + PI / 2); low1 = np.cos(nang - PI / 2)
        hc0 = low0.copy(); hc1 = low1.copy()
        for j in range(steps):
            t = ((j + noise2) / steps) ** 1.5              # denser near the pixel
            off = np.maximum(t * radPx, j + 1.0)           # at least one pixel per step
            ou = dx * off / W; ov = dy * off / H
            for side, hc, low in ((0, hc0, low0), (1, hc1, low1)):
                sgn_s = 1 if side == 0 else -1
                su = u + sgn_s * ou; sv = v + sgn_s * ov
                inside = (su > 0) & (su < 1) & (sv > 0) & (sv < 1)
                sp = P(su, sv, D(su, sv))
                delta = sp - p
                if qeps:
                    ul = 2.0 ** (np.floor(np.log2(np.maximum(d, 1))) - 10)
                    delta = delta - n * (qeps * ul)[..., None]
                dist = np.linalg.norm(delta, axis=-1) + 1e-6
                shc = np.sum(delta / dist[..., None] * viewV, -1)
                wgt = np.clip(dist * fMul + fAdd, 0, 1) * inside
                shc = low + (shc - low) * wgt
                np.maximum(hc, shc, out=hc)
        DBG.setdefault('hc',[]).append((hc0.copy(),hc1.copy(),nang.copy(),projLen.copy()))
        h0 = -np.arccos(np.clip(hc1, -1, 1)); h1 = np.arccos(np.clip(hc0, -1, 1))
        h0 = nang + np.maximum(h0 - nang, -PI / 2); h1 = nang + np.minimum(h1 - nang, PI / 2)
        sinN = np.sin(nang)
        iarc0 = (cosN + 2 * h0 * sinN - np.cos(2 * h0 - nang)) / 4
        iarc1 = (cosN + 2 * h1 * sinN - np.cos(2 * h1 - nang)) / 4
        vis += projLen * (iarc0 + iarc1)
    vis = np.clip(vis / slices, 0, 1) ** power
    vis = vis + (1 - vis) * np.clip((d - maxd * 0.6) / (maxd * 0.4), 0, 1)
    vis = 1 + (vis - 1) * np.clip((d - near) / near, 0, 1)
    return vis
if __name__ == '__main__':
    dep = load_dds_fp16(sys.argv[1])[..., 3][::2, ::2]
    smooth = len(sys.argv) < 4 or sys.argv[3] != 'nosmooth'
    qe = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    ao = blur(gtao(dep, smooth_normals=smooth, qeps=qe), dep, 2)
    save(ao, sys.argv[2]); print('min %.3f mean %.3f' % (ao.min(), ao.mean()))
