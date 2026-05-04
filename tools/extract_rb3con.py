#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
from pathlib import Path


BLOCK_SIZE = 0x1000
DATA_BASE_OFFSET = 0xC000
FILE_ENTRY_SIZE = 0x40
FILE_TABLE_BLOCK = 0


@dataclass
class FileEntry:
    index: int
    name: str
    is_directory: bool
    is_contiguous: bool
    block_count: int
    start_block: int
    parent_index: int
    size: int


def read_int24_le(value: bytes) -> int:
    return int.from_bytes(value, "little")


def block_to_offset(block_number: int) -> int:
    # STFS block numbers count only data blocks. Hash-table blocks are stored in
    # the file but are not counted by file entries, so we skip over them here.
    adjusted_block = block_number

    if block_number >= 0xAA:
        adjusted_block += (block_number // 0xAA) + 1

    if block_number > 0x70E4:
        adjusted_block += (block_number // 0x70E4) + 1

    return DATA_BASE_OFFSET + adjusted_block * BLOCK_SIZE


def parse_file_table(package: bytes) -> list[FileEntry]:
    table_offset = block_to_offset(FILE_TABLE_BLOCK)
    entries: list[FileEntry] = []

    for index in range(BLOCK_SIZE // FILE_ENTRY_SIZE):
        offset = table_offset + index * FILE_ENTRY_SIZE
        raw_entry = package[offset : offset + FILE_ENTRY_SIZE]

        if raw_entry == b"\0" * FILE_ENTRY_SIZE:
            if entries:
                break
            continue

        flags = raw_entry[0x28]
        name_length = flags & 0x3F
        raw_name = raw_entry[:0x28]
        name = raw_name[:name_length].decode("latin1", errors="replace")

        if not name:
            continue

        entries.append(
            FileEntry(
                index=index,
                name=name,
                is_directory=bool(flags & 0x80),
                is_contiguous=bool(flags & 0x40),
                block_count=read_int24_le(raw_entry[0x29:0x2C]),
                start_block=read_int24_le(raw_entry[0x2F:0x32]),
                parent_index=int.from_bytes(raw_entry[0x32:0x34], "big"),
                size=int.from_bytes(raw_entry[0x34:0x38], "big"),
            )
        )

    return entries


def build_directory_path(entries_by_index: dict[int, FileEntry], index: int) -> Path:
    if index == 0xFFFF:
        return Path()

    entry = entries_by_index[index]
    parent_path = build_directory_path(entries_by_index, entry.parent_index)
    return parent_path / entry.name


def read_entry_data(package: bytes, entry: FileEntry) -> bytes:
    chunks: list[bytes] = []
    bytes_left = entry.size

    for block_offset in range(entry.block_count):
        block_number = entry.start_block + block_offset
        package_offset = block_to_offset(block_number)
        chunk_size = min(BLOCK_SIZE, bytes_left)
        chunks.append(package[package_offset : package_offset + chunk_size])
        bytes_left -= chunk_size

        if bytes_left <= 0:
            break

    return b"".join(chunks)


def clean_file_name(entry_name: str) -> str:
    lower_name = entry_name.lower()

    if lower_name == "songs.dta":
        return "metadata.dta"

    if lower_name.endswith(".mid"):
        return "notes.mid"

    if lower_name.endswith(".mogg"):
        return "song.mogg"

    if lower_name.endswith("_keep.png_xbox") or lower_name.endswith(".png_xbox"):
        return "album.png_xbox"

    if lower_name.endswith(".milo_xbox"):
        return "venue.milo_xbox"

    return entry_name


def should_treat_as_audio(entry_name: str) -> bool:
    return Path(entry_name).suffix.lower() in (".mogg", ".ogg", ".wav", ".mp3", ".m4a", ".aiff")


def output_path_for_entry(
    entry: FileEntry,
    output_dir: Path,
    audio_output_dir: Path | None,
    preserve_paths: bool,
) -> Path:
    cleaned_name = clean_file_name(entry.name)

    if audio_output_dir is not None and should_treat_as_audio(entry.name):
        return audio_output_dir / f"{output_dir.name}{Path(cleaned_name).suffix}"

    if not preserve_paths:
        return output_dir / cleaned_name

    return output_dir / cleaned_name


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(Path.cwd()))
    except ValueError:
        return str(path)


def extract_package(
    package_path: Path,
    output_dir: Path,
    write_ogg_copy: bool,
    audio_output_dir: Path | None,
    preserve_paths: bool,
) -> None:
    package = package_path.read_bytes()

    if package[:4] not in (b"CON ", b"LIVE", b"PIRS"):
        raise ValueError("input does not look like an Xbox 360 STFS package")

    entries = parse_file_table(package)
    entries_by_index = {entry.index: entry for entry in entries}

    output_dir.mkdir(parents=True, exist_ok=True)
    if audio_output_dir is not None:
        audio_output_dir.mkdir(parents=True, exist_ok=True)

    for entry in entries:
        if entry.is_directory or entry.block_count == 0:
            continue

        if preserve_paths:
            parent_path = build_directory_path(entries_by_index, entry.parent_index)
            destination = output_dir / parent_path / entry.name
        else:
            destination = output_path_for_entry(entry, output_dir, audio_output_dir, preserve_paths)

        destination.parent.mkdir(parents=True, exist_ok=True)

        data = read_entry_data(package, entry)
        destination.write_bytes(data)

        print(f"extracted {display_path(destination)} ({entry.size} bytes)")

        if write_ogg_copy and destination.suffix.lower() == ".mogg":
            ogg_offset = data.find(b"OggS")
            if ogg_offset >= 0:
                ogg_destination = destination.with_suffix(".ogg")
                ogg_destination.write_bytes(data[ogg_offset:])
                print(f"extracted {display_path(ogg_destination)} (stripped Ogg copy)")


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract files from an RB3CON/STFS package.")
    parser.add_argument("package", type=Path, help="Path to the .rb3con/.con package")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output folder. Defaults to ./extracted/<package name>",
    )
    parser.add_argument(
        "--write-ogg-copy",
        action="store_true",
        help="Also write a plain .ogg copy by stripping the MOGG header at the first OggS page.",
    )
    parser.add_argument(
        "--audio-output",
        type=Path,
        help="Optional folder for extracted audio files. Useful for keeping charts/ free of playable audio.",
    )
    parser.add_argument(
        "--preserve-paths",
        action="store_true",
        help="Keep the original Rock Band folder paths and file names instead of clean names.",
    )
    args = parser.parse_args()

    package_path = args.package.expanduser().resolve()
    output_dir = args.output
    if output_dir is None:
        output_dir = Path("extracted") / package_path.stem

    audio_output_dir = args.audio_output
    if audio_output_dir is not None:
        audio_output_dir = audio_output_dir.expanduser().resolve()

    extract_package(
        package_path,
        output_dir.expanduser().resolve(),
        args.write_ogg_copy,
        audio_output_dir,
        args.preserve_paths,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
