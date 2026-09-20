#!/usr/bin/env python3
"""blutter 输出的归一化对比（用于校验展示层改动是否"仅增强、无结构漂移"）。

实例描述里有三类"有意/环境相关"差异，比较前需剥离：
  1. 库前缀（C 增强）：Obj![lib] Name        -> Obj!Name
  2. enum 常量名（D 增强）：Obj!Name.value@  -> Obj!Name@
  3. 运行期地址（ASLR）与随机大数

剥离以上后若两份输出仍逐字节一致，说明改动只是展示层追加，没有改变
asm 控制流/池引用等结构。

用法：
  python3 scripts/norm_diff.py <baseline_dir> <current_dir>
"""
import difflib
import pathlib
import re
import sys

STRIP_LIB_PREFIX = re.compile(r"Obj!\[[^\]]*\] ")
STRIP_ENUM_SUFFIX = re.compile(r"Obj!([A-Za-z0-9_$]+)\.[A-Za-z0-9_$]+@")
ADDR_LONG = re.compile(r"0x[0-9a-f]{5,}")
AT_ADDR = re.compile(r"@[0-9a-f]{4,}")
BIG_NUM = re.compile(r"\b\d{10,}\b")


def normalize(text: str) -> str:
    # 先剥离 C/D 增强，再归一化与环境相关的地址/大数
    text = STRIP_LIB_PREFIX.sub("Obj!", text)
    text = STRIP_ENUM_SUFFIX.sub(r"Obj!\1@", text)
    text = ADDR_LONG.sub("0xADDR", text)
    text = AT_ADDR.sub("@ADDR", text)
    text = BIG_NUM.sub("NUM", text)
    return text


def rel_files(root: pathlib.Path):
    return {str(p.relative_to(root)): p for p in root.rglob("*") if p.is_file()}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    base = pathlib.Path(sys.argv[1])
    cur = pathlib.Path(sys.argv[2])
    bf = rel_files(base)
    cf = rel_files(cur)
    drifted = []
    only_base = sorted(set(bf) - set(cf))
    only_cur = sorted(set(cf) - set(bf))
    for name in sorted(set(bf) & set(cf)):
        bt = normalize(bf[name].read_text(errors="replace"))
        ct = normalize(cf[name].read_text(errors="replace"))
        if bt != ct:
            diff = list(difflib.unified_diff(bt.splitlines(), ct.splitlines(), lineterm="", n=1))
            drifted.append((name, len(diff), diff[:40]))
    print(f"common files: {len(set(bf) & set(cf))}, drifted: {len(drifted)}")
    print(f"only in baseline: {len(only_base)}, only in current: {len(only_cur)}")
    for n in only_base[:20]:
        print(f"  - {n}")
    for n in only_cur[:20]:
        print(f"  + {n}")
    for name, n, sample in drifted[:10]:
        print(f"\n=== {name} ({n} diff lines) ===")
        for line in sample:
            print(line)
    if drifted:
        print(f"\nTOTAL drifted files: {len(drifted)}")
        sys.exit(1)
    print("\nNO DRIFT (after stripping C/D enrichment and addresses)")


if __name__ == "__main__":
    main()
