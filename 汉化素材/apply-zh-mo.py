#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OrcaSlicer-ImageMap 中文补丁：把 zh-tsv.tsv 里的翻译合并进应用的 gettext 目录文件。

用法（在应用关闭时执行）：
    python apply-zh-mo.py

原理：
  * 只“新增”条目，绝不覆盖应用已有的翻译（字节级判重）；
  * 保留原文件的元数据（charset/Plural-Forms）与复数形式条目；
  * msgid 按字节序排序（GNU MO 要求），msgstr 偏移按完整字符串区计算。

升级应用后（resources/ 被覆盖）重新跑一次本脚本即可。
"""
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # orcaslicer-imagemap 目录
MO = os.path.join(ROOT, "OrcaSlicer", "resources", "i18n", "zh_CN", "OrcaSlicer.mo")
TSV = os.path.join(HERE, "zh-tsv.tsv")
BAK = os.path.join(HERE, "OrcaSlicer.mo.原始备份")


def read_entries(path):
    d = open(path, "rb").read()
    magic, rev, n, o_off, t_off, hsz, hoff = struct.unpack("<7I", d[:28])
    if magic != 0x950412DE:
        raise SystemExit("不是有效的 MO 文件: " + path)
    out = []
    for i in range(n):
        l1, o1 = struct.unpack("<2I", d[o_off + 8 * i:o_off + 8 * i + 8])
        l2, o2 = struct.unpack("<2I", d[t_off + 8 * i:t_off + 8 * i + 8])
        out.append((d[o1:o1 + l1], d[o2:o2 + l2]))
    return out


def write_mo(path, entries):
    N = len(entries)
    o_off, t_off = 28, 28 + 8 * N
    data_off = t_off + 8 * N
    ids = b"".join(k + b"\x00" for k, _ in entries)
    strs = b"".join(v + b"\x00" for _, v in entries)
    ot, tt, pos = [], [], 0
    for k, _ in entries:
        ot.append((len(k), data_off + pos))
        pos += len(k) + 1
    pos = 0
    for _, v in entries:
        tt.append((len(v), data_off + len(ids) + pos))
        pos += len(v) + 1
    buf = struct.pack("<7I", 0x950412DE, 0, N, o_off, t_off, 0, 0)
    for l, o in ot:
        buf += struct.pack("<2I", l, o)
    for l, o in tt:
        buf += struct.pack("<2I", l, o)
    open(path, "wb").write(buf + ids + strs)


def main():
    if not os.path.exists(MO):
        raise SystemExit("找不到: " + MO)
    if not os.path.exists(BAK):
        shutil.copy2(MO, BAK)
        print("已备份原始 .mo ->", os.path.basename(BAK))
    entries = read_entries(MO)
    have = {k for k, _ in entries}
    before = len(entries)
    added = skipped = 0
    for raw in open(TSV, encoding="utf-8-sig").read().splitlines():
        if not raw.strip() or "\t" not in raw:
            continue
        k, v = raw.split("\t", 1)
        kb = k.encode("utf-8")
        if kb in have:
            skipped += 1
            continue
        entries.append((kb, v.encode("utf-8")))
        have.add(kb)
        added += 1
    entries.sort(key=lambda kv: kv[0])
    write_mo(MO, entries)
    print(f"完成: {before} -> {len(entries)} 条（新增 {added}，已存在跳过 {skipped}）")
    print("重启 OrcaSlicer-ImageMap 后生效。")


if __name__ == "__main__":
    sys.exit(main())
