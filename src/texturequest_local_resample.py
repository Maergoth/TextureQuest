from typing import Tuple
import numpy as np
import cv2

def resample(px: np.ndarray, scale: int, method: str = "lanczos", wrap: Tuple[bool, bool] = (False, False)) -> np.ndarray:
    """Deterministic resampling for data textures. px: uint8 or float32 (H, W, C)."""
    h, w = px.shape[:2]
    c = px.shape[2] if px.ndim == 3 else 1
    src = px.reshape(h, w, c)
    pad = 8
    mode_x = "wrap" if wrap[0] else "reflect"
    mode_y = "wrap" if wrap[1] else "reflect"
    f = np.pad(src.astype(np.float32), ((0, 0), (pad, pad), (0, 0)), mode=mode_x)
    f = np.pad(f, ((pad, pad), (0, 0), (0, 0)), mode=mode_y)
    interp = {"lanczos": cv2.INTER_LANCZOS4, "cubic": cv2.INTER_CUBIC, "linear": cv2.INTER_LINEAR, "area": cv2.INTER_AREA}[method]
    outs = []
    for i in range(0, c, 4):
        chunk = f[..., i:i + 4]
        r = cv2.resize(chunk, (f.shape[1] * scale, f.shape[0] * scale), interpolation=interp)
        if r.ndim == 2:
            r = r[..., None]
        outs.append(r)
    r = np.concatenate(outs, axis=-1)
    r = r[pad * scale:(pad + h) * scale, pad * scale:(pad + w) * scale]
    if px.dtype == np.uint8:
        return np.clip(np.rint(r), 0, 255).astype(np.uint8)
    return r.astype(np.float32)

