"""Map image utilities: PGM to PNG conversion and metadata extraction."""

import os
import io
import yaml
from PIL import Image


def find_map_yaml(map_dir):
    """Find the map .yaml file in a map directory.

    Looks for .yaml files that contain an 'image:' field (ROS2 map format).
    Returns the first match, or None.
    """
    for fname in sorted(os.listdir(map_dir)):
        if not fname.endswith('.yaml'):
            continue
        fpath = os.path.join(map_dir, fname)
        try:
            with open(fpath, 'r') as f:
                data = yaml.safe_load(f)
            if isinstance(data, dict) and 'image' in data:
                return fpath, data
        except Exception:
            continue
    return None, None


def get_map_metadata(map_dir):
    """Get map metadata (resolution, origin, image size) from a map directory.

    Returns dict with keys: resolution, origin, width, height, yaml_path, image_path
    or None if not found.
    """
    yaml_path, data = find_map_yaml(map_dir)
    if data is None:
        return None

    image_file = data['image']
    image_path = os.path.join(map_dir, image_file)
    if not os.path.exists(image_path):
        return None

    img = Image.open(image_path)
    width, height = img.size

    return {
        'resolution': data.get('resolution', 0.05),
        'origin': data.get('origin', [0.0, 0.0, 0.0]),
        'width': width,
        'height': height,
        'yaml_path': yaml_path,
        'image_path': image_path,
    }


def get_map_png(map_dir):
    """Convert the map image (PGM) to PNG bytes.

    Returns (png_bytes, content_type) or (None, None) if not found.
    """
    meta = get_map_metadata(map_dir)
    if meta is None:
        return None, None

    img = Image.open(meta['image_path'])
    # Convert to RGB for consistent PNG output
    if img.mode != 'RGB':
        img = img.convert('RGB')

    buf = io.BytesIO()
    img.save(buf, format='PNG')
    buf.seek(0)
    return buf.getvalue(), 'image/png'
