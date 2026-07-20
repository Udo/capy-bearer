#!/usr/bin/env python3
"""Capy compiler command-line entry point."""

import argparse
import json
from pathlib import Path
import sys

from capy_frontend import *
from capy_backend import *

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="capyc", description="Compile Capy directly to WebAssembly")
    parser.add_argument("source")
    parser.add_argument("-o", "--output")
    parser.add_argument("--source-map")
    parser.add_argument("--dump-ast", action="store_true")
    parser.add_argument("--bearer-unit", action="store_true")
    parser.add_argument("--abi-version", type=int)
    args = parser.parse_args(argv)
    path = Path(args.source)
    try:
        program = parse(path.read_text(), str(path))
        DeclarationIndex().add_program(program)
        if args.dump_ast:
            print(json.dumps(ast_json(program), indent=2))
            return 0
        if args.bearer_unit:
            if not args.output or not args.source_map or args.abi_version is None:
                parser.error("--bearer-unit requires --output, --source-map, and --abi-version")
            wasm, source_map = compile_bearer_unit(program, str(path.resolve()), Path(args.output).name, args.abi_version)
            Path(args.output).write_bytes(wasm)
            Path(args.source_map).write_text(source_map)
            return 0
    except (OSError, CapyError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print("capyc: select --dump-ast or --bearer-unit", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
