"""Automatically select shared textures; never rewrite texture files.

Usage: python tools/select_shared_textures.py texture-index.json choices.json
       [--correction named-id:revision ...]
Existing correction priorities are carried forward automatically.
"""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from texturequest_shared_textures import propose


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('index', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--previous', type=Path, default=Path(__file__).resolve().parents[1]/'src/shared-texture-choices.json')
    parser.add_argument('--correction', action='append', default=[])
    args = parser.parse_args()
    priorities = json.loads(args.previous.read_bytes()).get('correction_priority', {}) if args.previous.is_file() else {}
    for value in args.correction:
        key, revision = value.rsplit(':', 1)
        priorities[key] = int(revision)
    choices = propose(json.loads(args.index.read_bytes()), priorities)
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(choices, stream, indent=2); stream.write('\n')
