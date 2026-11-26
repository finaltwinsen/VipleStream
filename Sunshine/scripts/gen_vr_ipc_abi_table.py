#!/usr/bin/env python3
# §VRDUAL-4A：由 vr_ipc_abi.h 產生／檢查 vr_ipc_abi_table.h。
# 建置時跑 --check（cmake 的 vripc-abi-check、Build-SteamVRDriver.ps1）。
# 標頭加了整數巨集或 struct 欄位卻沒更新表，--check 以非 0 結束，建置失敗。
# --write 會重寫表的 k_rows。不改結構、不改 ABI 版號。
"""產生並檢查 VR IPC ABI 對照表。"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "vr" / "vr_ipc_abi.h"
TABLE = ROOT / "src" / "vr" / "vr_ipc_abi_table.h"

_INT = re.compile(r"(?:0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\Z")
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(VRIPC_[A-Za-z0-9_]+)[ \t]+(\S(?:.*\S)?)\s*$")
_STRUCT = re.compile(r"typedef\s+struct\s+(vripc_[A-Za-z0-9_]+)\s*\{")
_ROW = re.compile(
    r"VRIPC_ABI_ROW_(CONST|SIZE|ALIGN)\(\s*([A-Za-z0-9_]+)\s*\)"
    r"|VRIPC_ABI_ROW_OFF\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)"
)


def _strip_comments(text: str) -> str:
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise SystemExit("vr_ipc_abi.h：區塊註解沒有結束")
            out.append(" ")
            i = j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            if j < 0:
                break
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _join_continuations(text: str) -> str:
    return text.replace("\\\n", " ").replace("\\\r\n", " ")


def _split_commas(stmt: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in stmt:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def _field_names(stmt: str) -> list[str]:
    names: list[str] = []
    for part in _split_commas(stmt):
        part = re.sub(r"(\s*\[[^\]]*\])+\s*$", "", part.strip())
        match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", part)
        if not match:
            raise SystemExit(f"vr_ipc_abi.h：無法解析欄位：{stmt!r}")
        names.append(match.group(1))
    return names


def parse_header(text: str) -> tuple[list[str], list[tuple[str, list[str]]]]:
    body = _join_continuations(_strip_comments(text))
    consts: list[str] = []
    seen: set[str] = set()
    for line in body.splitlines():
        match = _DEFINE.match(line)
        if not match:
            continue
        name, value = match.group(1), match.group(2).strip()
        if name in seen:
            raise SystemExit(f"vr_ipc_abi.h：巨集重複：{name}")
        seen.add(name)
        if not _INT.fullmatch(value):
            # 字串、函式巨集、或不是整數常數。表只列整數，漏掉的不能靜靜略過未知的整數。
            continue
        consts.append(name)

    structs: list[tuple[str, list[str]]] = []
    for match in _STRUCT.finditer(body):
        name = match.group(1)
        brace = match.end() - 1
        depth = 0
        end = None
        for i in range(brace, len(body)):
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end is None:
            raise SystemExit(f"vr_ipc_abi.h：struct 沒有結束：{name}")
        fields: list[str] = []
        chunk = body[brace + 1 : end]
        depth = 0
        cur: list[str] = []
        for ch in chunk:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            if ch == ";" and depth == 0:
                stmt = "".join(cur).strip()
                cur = []
                if not stmt:
                    continue
                if stmt.startswith("#"):
                    raise SystemExit(f"vr_ipc_abi.h：{name} 裡有前處理器指令，產生器不展開：{stmt}")
                fields.extend(_field_names(stmt))
            else:
                cur.append(ch)
        leftover = "".join(cur).strip()
        if leftover:
            raise SystemExit(f"vr_ipc_abi.h：{name} 有沒有分號的內容：{leftover!r}")
        if not fields:
            raise SystemExit(f"vr_ipc_abi.h：{name} 沒有欄位")
        structs.append((name, fields))
    if not structs:
        raise SystemExit("vr_ipc_abi.h：沒有解析到任何 vripc struct")
    return consts, structs


def expected_keys(consts: list[str], structs: list[tuple[str, list[str]]]) -> list[str]:
    keys: list[str] = [f"const.{name}" for name in consts]
    for name, fields in structs:
        keys.append(f"size.{name}")
        keys.append(f"align.{name}")
        keys.extend(f"off.{name}.{field}" for field in fields)
    if len(keys) != len(set(keys)):
        raise SystemExit("vr_ipc_abi.h：產生的 ABI 列鍵重複")
    return keys


def table_keys(text: str) -> list[str]:
    # 只看 k_rows 那一段：檔頭的 #define VRIPC_ABI_ROW_*(c)／(s, f) 巨集定義本身也長得像一列
    idx = text.find("inline constexpr row_t k_rows[]")
    if idx >= 0:
        end = text.find("};", idx)
        text = text[idx : end if end >= 0 else len(text)]
    keys: list[str] = []
    for match in _ROW.finditer(text):
        kind = match.group(1)
        if kind == "CONST":
            keys.append(f"const.{match.group(2)}")
        elif kind == "SIZE":
            keys.append(f"size.{match.group(2)}")
        elif kind == "ALIGN":
            keys.append(f"align.{match.group(2)}")
        else:
            keys.append(f"off.{match.group(3)}.{match.group(4)}")
    return keys


def render_rows(consts: list[str], structs: list[tuple[str, list[str]]], nl: str) -> str:
    lines = [
        "  // §VRDUAL-4A：scripts/gen_vr_ipc_abi_table.py 由 vr_ipc_abi.h 產生。"
        f"{len(structs)} 個 struct、{len(expected_keys(consts, structs))} 列。"
        "不要手改；加欄位或整數巨集後執行 --write，建置的 --check 會擋下漏列。",
        "  inline constexpr row_t k_rows[] = {",
    ]
    for name in consts:
        lines.append(f"      VRIPC_ABI_ROW_CONST({name}),")
    for name, fields in structs:
        lines.append(f"      VRIPC_ABI_ROW_SIZE({name}),")
        lines.append(f"      VRIPC_ABI_ROW_ALIGN({name}),")
        for field in fields:
            lines.append(f"      VRIPC_ABI_ROW_OFF({name}, {field}),")
    lines.append("  };")
    return nl.join(lines)


def replace_rows(text: str, rendered: str) -> str:
    idx = text.find("inline constexpr row_t k_rows[]")
    if idx < 0:
        raise SystemExit("vr_ipc_abi_table.h：找不到 k_rows")
    line_start = text.rfind("\n", 0, idx)
    line_start = 0 if line_start < 0 else line_start + 1
    prev_end = line_start - 1
    if prev_end > 0:
        prev_start = text.rfind("\n", 0, prev_end - 1)
        prev_start = 0 if prev_start < 0 else prev_start + 1
        prev = text[prev_start:line_start].lstrip()
        if prev.startswith("//"):
            line_start = prev_start
    end_stmt = text.find("};", idx)
    if end_stmt < 0:
        raise SystemExit("vr_ipc_abi_table.h：k_rows 沒有結束")
    return text[:line_start] + rendered + text[end_stmt + 2 :]


def main() -> int:
    parser = argparse.ArgumentParser(description="產生或檢查 VR IPC ABI 表（§VRDUAL-4A）")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="表與標頭不一致就以 exit 1 結束")
    mode.add_argument("--write", action="store_true", help="依標頭重寫 k_rows")
    args = parser.parse_args()

    header_text = HEADER.read_text(encoding="utf-8")
    table_text = TABLE.read_text(encoding="utf-8")
    nl = "\r\n" if "\r\n" in table_text else "\n"
    consts, structs = parse_header(header_text)
    want = expected_keys(consts, structs)
    have = table_keys(table_text)

    if args.check:
        if have == want:
            print(
                f"[VRIPC-ABI] ok structs={len(structs)} rows={len(want)} consts={len(consts)}",
                file=sys.stderr,
            )
            return 0
        missing = [key for key in want if key not in set(have)]
        extra = [key for key in have if key not in set(want)]
        print(
            f"[VRIPC-ABI] mismatch table={len(have)} header={len(want)} "
            f"missing={len(missing)} extra={len(extra)}",
            file=sys.stderr,
        )
        for key in missing[:40]:
            print(f"  missing {key}", file=sys.stderr)
        for key in extra[:40]:
            print(f"  extra {key}", file=sys.stderr)
        if have != want and not missing and not extra:
            print("  列鍵相同但順序不同", file=sys.stderr)
        print("  修表：python scripts/gen_vr_ipc_abi_table.py --write", file=sys.stderr)
        return 1

    updated = replace_rows(table_text, render_rows(consts, structs, nl))
    if not updated.endswith(nl):
        updated += nl
    if updated != table_text:
        TABLE.write_text(updated, encoding="utf-8", newline="")
        print(f"[VRIPC-ABI] wrote {TABLE} rows={len(want)}", file=sys.stderr)
    else:
        print(f"[VRIPC-ABI] unchanged rows={len(want)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
