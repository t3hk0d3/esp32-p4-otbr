"""Preview PNGs: orthographic, depth-buffered point splats of the meshes (numpy only; matplotlib writes the PNG).

Each mesh is sampled densely on its surface; points are projected for the view, the nearest point per pixel wins, and
it is shaded by its face normal. No GUI or OpenGL is needed, and there are no painter's-algorithm artefacts.
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import trimesh

COLOURS = {
    "base": (0.80, 0.82, 0.85),
    "lid": (0.58, 0.66, 0.98),
    "board": (0.25, 0.62, 0.38),
}
BACKGROUND = (1.0, 1.0, 1.0)
PX_PER_MM = 8
POINTS_PER_MM2 = 160
MARGIN_MM = 3.0
# view name: (direction from the scene towards the viewer, screen-up direction)
VIEWS = {
    "front": ((0, 0, 1), (0, 1, 0)),
    "back": ((0, 0, -1), (0, 1, 0)),
    "cable_end": ((0, -1, 0), (0, 0, 1)),
    "header_end": ((0, 1, 0), (0, 0, 1)),
    "button_side": ((-1, 0, 0), (0, 0, 1)),
    "exploded": ((-1.0, -1.3, 1.1), (0, 0, 1)),
}


def _camera(view: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    to_viewer, up = (np.asarray(v, float) for v in VIEWS[view])
    to_viewer /= np.linalg.norm(to_viewer)
    right = np.cross(up, to_viewer)
    right /= np.linalg.norm(right)
    up = np.cross(to_viewer, right)
    return right, up, to_viewer


def render(meshes: list[tuple[trimesh.Trimesh, str]], view: str, path: Path) -> Path:
    """Draws (mesh, colour key) pairs from one named view into a PNG."""
    right, up, to_viewer = _camera(view)
    light = to_viewer + 0.6 * up - 0.4 * right
    light /= np.linalg.norm(light)
    pts, rgb = [], []
    for mesh, key in meshes:
        n = max(int(mesh.area * POINTS_PER_MM2), 1000)
        p, face = trimesh.sample.sample_surface(mesh, n, seed=3)
        normals = mesh.face_normals[face]
        shade = 0.35 + 0.65 * np.clip(normals @ light, 0, 1)
        pts.append(p)
        rgb.append(np.outer(shade, COLOURS[key]))
    pts, rgb = np.vstack(pts), np.vstack(rgb)
    u, v, depth = pts @ right, pts @ up, pts @ to_viewer
    u0, v0 = u.min() - MARGIN_MM, v.min() - MARGIN_MM
    w = int((u.max() + MARGIN_MM - u0) * PX_PER_MM) + 1
    h = int((v.max() + MARGIN_MM - v0) * PX_PER_MM) + 1
    px = ((u - u0) * PX_PER_MM).astype(int)
    py = (h - 1 - (v - v0) * PX_PER_MM).astype(int)
    order = np.argsort(depth)  # far to near: the nearest point is written last and wins
    image = np.ones((h, w, 3)) * BACKGROUND
    image[py[order], px[order]] = rgb[order]
    plt.imsave(path, np.clip(image, 0, 1))
    return path
