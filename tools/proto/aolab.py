# Offline replica of the APRenewal SSAO shaders, run on real captured depth (HDR alpha).
import numpy as np, struct, sys
from PIL import Image

def load_dds_fp16(p):
    b = open(p, 'rb').read(); h, w = struct.unpack('<2I', b[12:20])
    return np.frombuffer(b[128:128 + w*h*8], dtype=np.float16).reshape(h, w, 4).astype(np.float32)

def sampler(depth):
    H, W = depth.shape
    def D(u, v):   # point sampling, clamp addressing (like the shader)
        x = np.clip((u * W).astype(np.int32), 0, W - 1); y = np.clip((v * H).astype(np.int32), 0, H - 1)
        return depth[y, x]
    return D

def noise_ign(px, py):
    return np.modf(52.9829189 * np.modf(px * 0.06711056 + py * 0.00583715)[0])[0]

def blur(ao, depth, rad=4):
    out = ao.copy()
    for axis in (1, 0):
        acc = np.zeros_like(ao); wsum = np.zeros_like(ao)
        for i in range(-rad, rad + 1):
            a = np.roll(out, i, axis=axis); sd = np.roll(depth, i, axis=axis)
            w = np.exp(-(i * i) / 12.5) * np.clip(1 - np.abs(sd - depth) / (depth * 0.04 + 1), 0, 1)
            acc += a * w; wsum += w
        out = acc / np.maximum(wsum, 1e-4)
    return out

def save(img, path):
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(path)

def depth_denoise(depth, rad=6, rel=0.015):
    # separable edge-preserving average: turns fp16 staircases back into ramps, keeps object edges
    out = depth.copy()
    for axis in (1, 0):
        acc = np.zeros_like(out); ws = np.zeros_like(out)
        for i in range(-rad, rad + 1):
            sd = np.roll(out, i, axis=axis)
            w = (np.abs(sd - out) < out * rel).astype(np.float32)
            acc += sd * w; ws += w
        out = acc / np.maximum(ws, 1e-6)
    return out
