#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch only the signed, pinned SDK 2.4 upscaling DLLs. No FG runtime updates."""
import argparse
import json
from pathlib import Path
from fsr_sdk_files import prepare as prepare_files, verified

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = json.loads((HERE / 'fsr24-upscaling.json').read_text(encoding='utf-8'))


def prepare(target, check=False, pin=PIN):
    return prepare_files(target, pin, check, user_agent='bbport-fsr24-upscaling')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dir', type=Path, default=ROOT / 'out/fsr24')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        print('SDK 2.4 upscaling verified:', prepare(args.dir, args.check))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
