#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch the pinned SDK 2.4 ML-FG effect independently of upscaling."""
import argparse
import json
from pathlib import Path
import fsr_sdk_files

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = json.loads((HERE / 'fsr24-fg402.json').read_text(encoding='utf-8'))


def prepare(target, check=False):
    return fsr_sdk_files.prepare(target, PIN, check, user_agent='bbport-mlfg-sdk24')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dir', type=Path, default=ROOT / 'out/fsr24')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        print('SDK 2.4 ML-FG verified:', prepare(args.dir, args.check))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
