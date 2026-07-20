#!/usr/bin/env python3
"""Extract a compact address-to-source table before a unit's DWARF is stripped."""

import argparse
import ast
import os
import re
import subprocess


DIRECTORY = re.compile(r'^include_directories\[\s*(\d+)\] = (".*")$')
FILE = re.compile(r'^file_names\[\s*(\d+)\]:$')
ROW = re.compile(r'^0x([0-9a-fA-F]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+')


def quoted(value: str) -> str:
	return ast.literal_eval(value)


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--dwarfdump", required=True)
	parser.add_argument("--wasm", required=True)
	parser.add_argument("--module", required=True)
	parser.add_argument("--output", required=True)
	args = parser.parse_args()

	text = subprocess.run(
		[args.dwarfdump, "--debug-line", args.wasm],
		check=True,
		stdout=subprocess.PIPE,
		text=True,
	).stdout
	directories: dict[int, str] = {}
	files: dict[int, tuple[str, int]] = {}
	rows: list[tuple[int, int, int, int]] = []
	current_file = 0
	for line in text.splitlines():
		if match := DIRECTORY.match(line):
			directories[int(match.group(1))] = quoted(match.group(2))
			continue
		if match := FILE.match(line):
			current_file = int(match.group(1))
			continue
		stripped = line.strip()
		if current_file and stripped.startswith("name: "):
			files[current_file] = (quoted(stripped[6:]), 0)
			continue
		if current_file and stripped.startswith("dir_index: "):
			name, _ = files[current_file]
			files[current_file] = (name, int(stripped[11:]))
			continue
		if match := ROW.match(line):
			rows.append(tuple(int(value, 16 if index == 0 else 10) for index, value in enumerate(match.groups())))

	paths: dict[int, str] = {}
	for file_id, (name, directory_id) in files.items():
		directory = directories.get(directory_id, "")
		path = name if os.path.isabs(name) else os.path.normpath(os.path.join(directory, name))
		if not os.path.isabs(path):
			path = os.path.abspath(path)
		if "\t" in path or "\n" in path or "\r" in path:
			raise ValueError(f"source-map path contains a control character: {path!r}")
		paths[file_id] = path

	with open(args.output, "w", encoding="utf-8") as output:
		output.write(f"UCE_SOURCE_MAP_V1\t{args.module}\n")
		for file_id, path in sorted(paths.items()):
			output.write(f"F\t{file_id}\t{path}\n")
		for address, line, column, file_id in rows:
			output.write(f"L\t{address:x}\t{file_id}\t{line}\t{column}\n")


if __name__ == "__main__":
	main()
