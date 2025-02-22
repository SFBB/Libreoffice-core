#!/usr/bin/env python3

import sys
import os
import subprocess
import json
import argparse

from tools import uncompress_file_to_dir, get_file_info, make_complete_mar_name
from signing import sign_mar_file
from path import UpdaterPath, convert_to_unix, convert_to_native

current_dir_path = os.path.dirname(os.path.realpath(convert_to_unix(__file__)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('product_name')
    parser.add_argument('workdir')
    parser.add_argument('filename_prefix')
    parser.add_argument('certificate_path')
    parser.add_argument('certificate_name')
    parser.add_argument('base_url')
    args = parser.parse_args()

    certificate_path = args.certificate_path
    certificate_name = args.certificate_name
    base_url = args.base_url
    filename_prefix = args.filename_prefix
    workdir = args.workdir
    product_name = args.product_name

    update_path = UpdaterPath(workdir)
    update_path.ensure_dir_exist()

    target_dir = update_path.get_update_dir()
    temp_dir = update_path.get_current_build_dir()

    tar_dir = os.path.join(update_path.get_workdir(), "installation", product_name, "archive", "install", "en-US")
    tar_file = os.path.join(tar_dir, os.listdir(tar_dir)[0])

    uncompress_dir = uncompress_file_to_dir(tar_file, temp_dir)

    mar_file = make_complete_mar_name(target_dir, filename_prefix)
    path = os.path.join(current_dir_path, 'make_full_update.sh')
    subprocess.call([path, convert_to_native(mar_file), convert_to_native(uncompress_dir)])

    sign_mar_file(target_dir, certificate_path, certificate_name, mar_file, filename_prefix)

    file_info = {'complete': get_file_info(mar_file, base_url)}

    with open(os.path.join(target_dir, 'complete_info.json'), "w") as complete_info_file:
        json.dump(file_info, complete_info_file, indent=4)


if __name__ == '__main__':
    main()
