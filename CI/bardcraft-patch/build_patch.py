#!/usr/bin/env python3

import argparse
import base64
import difflib
import hashlib
import json
from pathlib import Path


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_map(root: Path) -> dict[str, Path]:
    return {
        path.relative_to(root).as_posix(): path
        for path in root.rglob("*")
        if path.is_file() and ".git" not in path.parts
    }


def line_offsets(lines: list[bytes]) -> list[int]:
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    return offsets


def make_operations(source: bytes, output: bytes) -> list[dict[str, object]]:
    if not source:
        return [{"data": base64.b64encode(output).decode("ascii")}] if output else []

    source_lines = source.splitlines(keepends=True)
    output_lines = output.splitlines(keepends=True)
    offsets = line_offsets(source_lines)
    matcher = difflib.SequenceMatcher(None, source_lines, output_lines, autojunk=False)
    operations: list[dict[str, object]] = []

    for tag, source_start, source_end, output_start, output_end in matcher.get_opcodes():
        if tag == "equal":
            offset = offsets[source_start]
            length = offsets[source_end] - offset
            if length:
                operations.append({"copyOffset": offset, "copyLength": length})
        elif output_start != output_end:
            data = b"".join(output_lines[output_start:output_end])
            operations.append({"data": base64.b64encode(data).decode("ascii")})

    return operations


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the Fetcher Bardcraft multiplayer compatibility patch.")
    parser.add_argument("--vanilla-root", required=True, type=Path)
    parser.add_argument("--fetcher-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--applier", required=True, type=Path)
    parser.add_argument(
        "--previous-manifest",
        action="append",
        default=[],
        type=Path,
        help="Manifest whose verified outputs may be upgraded in place.",
    )
    args = parser.parse_args()

    vanilla_root = args.vanilla_root.resolve()
    fetcher_root = args.fetcher_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)

    vanilla_files = file_map(vanilla_root)
    fetcher_files = file_map(fetcher_root)
    previous_outputs: dict[str, set[str]] = {}
    for manifest_path in args.previous_manifest:
        manifest = json.loads(manifest_path.resolve().read_text(encoding="utf-8-sig"))
        for record in manifest["files"]:
            previous_outputs.setdefault(record["path"], set()).add(record["outputSha256"].lower())
    changed_paths = sorted(
        path
        for path in vanilla_files.keys() | fetcher_files.keys()
        if path not in vanilla_files
        or path not in fetcher_files
        or vanilla_files[path].read_bytes() != fetcher_files[path].read_bytes()
    )

    removed = [path for path in changed_paths if path not in fetcher_files]
    if removed:
        raise RuntimeError(f"Removed files are not supported: {', '.join(removed)}")

    records = []
    for relative_path in changed_paths:
        source = vanilla_files[relative_path].read_bytes() if relative_path in vanilla_files else b""
        output = fetcher_files[relative_path].read_bytes()
        source_hash = sha256(source) if relative_path in vanilla_files else None
        output_hash = sha256(output)
        prior_output_hashes = sorted(
            value
            for value in previous_outputs.get(relative_path, set())
            if value not in {source_hash, output_hash}
        )
        record = {
            "path": relative_path,
            "sourceSha256": source_hash,
            "sourceSize": len(source) if relative_path in vanilla_files else None,
            "outputSha256": output_hash,
            "outputSize": len(output),
            "operations": make_operations(source, output),
        }
        if prior_output_hashes:
            record["priorOutputSha256"] = prior_output_hashes
        records.append(record)

    manifest = {
        "formatVersion": 1,
        "patchVersion": args.version,
        "targetSubdirectory": "scripts/Bardcraft",
        "upstream": {
            "name": "Bardcraft (OpenMW)",
            "source": "Nexus Mods file named Bardcraft",
        },
        "files": records,
    }
    manifest_path = output_dir / "fetcher-bardcraft-mp-patch.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")

    applier_target = output_dir / "Apply-Fetcher-Bardcraft-MPPatch.ps1"
    applier_target.write_bytes(args.applier.resolve().read_bytes())

    readme = f"""Fetcher Bardcraft multiplayer compatibility patch {args.version}
============================================================

This package contains hash-gated deltas for the Nexus-installed Bardcraft
scripts. It does not contain Bardcraft meshes, sounds, MIDI files, plugins, or
an independently usable copy of the upstream mod.

Fetcher Bardcraft defaults to local song files with content-hash matching.
Community MIDI packs must be installed separately; this patch does not
download or redistribute song files.

Install Bardcraft through Nexus/UMO first. The Fetcher Simulator installer
normally applies this patch automatically. For manual use:

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\\Apply-Fetcher-Bardcraft-MPPatch.ps1 -BardcraftDataRoot "C:\\path\\to\\BardcraftOpenMW"

The applier validates every upstream file before changing anything, stages and
verifies all outputs, preserves backups under the Bardcraft data directory,
and refuses unsupported Bardcraft versions.
"""
    (output_dir / "README.txt").write_text(readme, encoding="ascii", newline="\r\n")

    total_source = sum(record["sourceSize"] or 0 for record in records)
    total_output = sum(record["outputSize"] for record in records)
    print(f"Built patch {args.version}: files={len(records)} sourceBytes={total_source} outputBytes={total_output}")
    for record in records:
        kind = "added" if record["sourceSha256"] is None else "modified"
        print(f"  {kind}: {record['path']}")


if __name__ == "__main__":
    main()
