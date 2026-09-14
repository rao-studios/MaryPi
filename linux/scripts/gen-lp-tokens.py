#!/usr/bin/env python3
"""Liquid Platinum's tokens for the Mac: reads LP_TOKENS[] from the MaryOS submodule's
maryui/include/maryui/lp_tokens.h (itself generated from MaryUI's tokens.json) and writes
Sources/LiquidPlatinum/LPTokens.swift, so a token change reaches the viewer with one
submodule bump and no MaryUI checkout or node.

  scripts/gen-lp-tokens.py               regenerate (make lp-tokens)
  scripts/gen-lp-tokens.py --check       exit 1 when the Swift file is out of date (make lp-tokens-check)
  scripts/gen-lp-tokens.py --out FILE    write somewhere else
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "maryos/maryui/include/maryui/lp_tokens.h"
OUT = ROOT / "Sources/LiquidPlatinum/LPTokens.swift"

KEYWORDS = {
    "associatedtype", "class", "deinit", "enum", "extension", "fileprivate", "func", "import", "init", "inout",
    "internal", "let", "open", "operator", "private", "protocol", "public", "rethrows", "static", "struct",
    "subscript", "typealias", "var", "break", "case", "catch", "continue", "default", "defer", "do", "else",
    "fallthrough", "for", "guard", "if", "in", "repeat", "return", "throw", "switch", "where", "while", "as",
    "false", "is", "nil", "self", "super", "throws", "true", "try", "await", "any", "some",
}
RESERVED_TYPES = {"Type", "Protocol", "Self", "Any"}


def fail(message):
    sys.exit(f"gen-lp-tokens: {message}")


def parse_header(text):
    sha = re.search(r'#define LP_TOKENS_SOURCE_SHA "([0-9a-f]+)"', text)
    rows = re.findall(r'\{ "([^"]+)", "([^"]+)", "([^"]+)", "((?:[^"\\]|\\.)*)" \}', text)
    if not sha or not rows:
        fail(f"no LP_TOKENS[] in {HEADER}")
    return sha.group(1), [(name, typ, value.replace('\\"', '"')) for name, _css, typ, value in rows]


def member(segment):
    parts = segment.split("-")
    name = parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
    if name[0].isdigit():
        name = "_" + name
    return f"`{name}`" if name in KEYWORDS else name


def type_name(segment):
    name = member(segment).strip("`")
    name = name[:1].upper() + name[1:]
    if name[0] == "_":
        name = "N" + name[1:]
    return name + "Group" if name in RESERVED_TYPES else name


def num(x):
    """Four decimals, as lp_tokens.h writes color components."""
    text = f"{x:.4f}".rstrip("0").rstrip(".")
    return text if text not in ("", "-0") else "0"


def precise(x):
    """The token's own value: the shortest literal that reads back as the same double."""
    return str(int(x)) if float(x).is_integer() else repr(float(x))


def color(value):
    v = value.strip()
    m = re.fullmatch(r"#([0-9a-fA-F]{3,8})", v)
    if m:
        h = m.group(1)
        if len(h) in (3, 4):
            h = "".join(c * 2 for c in h)
        r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
        a = int(h[6:8], 16) / 255 if len(h) == 8 else 1.0
        return f"LPColor(red: {num(r / 255)}, green: {num(g / 255)}, blue: {num(b / 255)}, alpha: {num(a)})"
    m = re.fullmatch(r"rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)", v)
    if m:
        r, g, b = (float(m.group(i)) for i in (1, 2, 3))
        a = float(m.group(4)) if m.group(4) is not None else 1.0
        return f"LPColor(red: {num(r / 255)}, green: {num(g / 255)}, blue: {num(b / 255)}, alpha: {num(a)})"
    if v == "transparent":
        return "LPColor(red: 0, green: 0, blue: 0, alpha: 0)"
    fail(f"a color this script does not read: {value}")


def length(value):
    m = re.fullmatch(r"(-?[\d.]+)(px|deg)?", value.strip())
    if not m:
        fail(f"a dimension this script does not read: {value}")
    return precise(float(m.group(1)))


def duration(value):
    m = re.fullmatch(r"([\d.]+)(ms|s)", value.strip())
    if not m:
        fail(f"a duration this script does not read: {value}")
    seconds = float(m.group(1)) / (1000 if m.group(2) == "ms" else 1)
    return precise(seconds)


def split_top(value):
    parts, depth, start = [], 0, 0
    for i, c in enumerate(value):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(value[start:i].strip())
            start = i + 1
    parts.append(value[start:].strip())
    return parts


def shadow(value):
    layers = []
    for layer in split_top(value):
        inset = layer.startswith("inset ")
        rest = layer[6:] if inset else layer
        m = re.fullmatch(r"(-?[\d.]+(?:px)?)\s+(-?[\d.]+(?:px)?)\s+([\d.]+(?:px)?)(?:\s+(-?[\d.]+(?:px)?))?\s+(.+)", rest)
        if not m:
            fail(f"a shadow layer this script does not read: {layer}")
        x, y, blur, spread = (length(m.group(i) or "0") for i in (1, 2, 3, 4))
        layers.append(f"LPShadowLayer(inset: {'true' if inset else 'false'}, x: {x}, y: {y}, blur: {blur}, spread: {spread}, color: {color(m.group(5))})")
    return "[" + ", ".join(layers) + "]"


def cubic(value):
    m = re.fullmatch(r"cubic-bezier\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)", value.strip())
    if not m:
        fail(f"a cubic-bezier this script does not read: {value}")
    x1, y1, x2, y2 = (num(float(m.group(i))) for i in (1, 2, 3, 4))
    return f"LPCubicBezier(x1: {x1}, y1: {y1}, x2: {x2}, y2: {y2})"


def swift_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def literal(typ, value):
    if typ == "color":
        return "LPColor", color(value)
    if typ == "number":
        return "Double", precise(float(value))
    if typ == "dimension":
        return "CGFloat", length(value)
    if typ == "duration":
        return "TimeInterval", duration(value)
    if typ == "fontWeight":
        return "Int", str(int(float(value)))
    if typ == "fontFamily":
        families = [f.strip().strip('"') for f in split_top(value)]
        return "[String]", "[" + ", ".join(swift_string(f) for f in families) + "]"
    if typ == "shadow":
        return "[LPShadowLayer]", shadow(value)
    if typ == "cubicBezier":
        return "LPCubicBezier", cubic(value)
    if typ == "string":
        return "String", swift_string(value)
    fail(f"a token type this script does not know: {typ}")


def generate():
    sha, tokens = parse_header(HEADER.read_text())
    tree = {"groups": {}, "leaves": []}
    for name, typ, value in tokens:
        segments = name.split(".")
        node = tree
        for segment in segments[:-1]:
            node = node["groups"].setdefault(segment, {"groups": {}, "leaves": []})
        swift_type, swift_value = literal(typ, value)
        node["leaves"].append((segments[-1], swift_type, swift_value, name, value))

    lines = [
        f"// GENERATED by scripts/gen-lp-tokens.py from maryos/maryui/include/maryui/lp_tokens.h (tokens.json sha256 {sha}).",
        "// Do not edit: change MaryUI's tokens.json, bump the maryos submodule, then `make lp-tokens`.",
        "import CoreGraphics",
        "import Foundation",
        "",
        "// swiftlint:disable all",
        "public enum LP {",
        f'    public static let sourceSHA = "{sha}"',
        f"    public static let count = {len(tokens)}",
    ]

    def emit(node, indent):
        pad = "    " * indent
        seen = set()
        for segment, swift_type, swift_value, name, value in node["leaves"]:
            identifier = member(segment)
            if identifier in seen:
                fail(f"two tokens make {identifier} in the same group ({name})")
            seen.add(identifier)
            comment = value if len(value) <= 60 else value[:57] + "..."
            lines.append(f"{pad}/// {name}: {comment}")
            lines.append(f"{pad}public static let {identifier}: {swift_type} = {swift_value}")
        for segment, child in node["groups"].items():
            identifier = type_name(segment)
            if identifier in seen:
                fail(f"a group and a token both make {identifier} ({segment})")
            seen.add(identifier)
            lines.append(f"{pad}public enum {identifier} {{")
            emit(child, indent + 1)
            lines.append(f"{pad}}}")

    emit(tree, 1)
    lines.append("")
    lines.append("    /// Every token as the header lists it, for tests and the About panel.")
    lines.append("    public static let all: [LPTokenInfo] = [")
    for name, typ, value in tokens:
        lines.append(f"        LPTokenInfo({swift_string(name)}, {swift_string(typ)}, {swift_string(value)}),")
    lines.append("    ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv):
    out = OUT
    check = False
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--check":
            check = True
        elif arg == "--out" and args:
            out = pathlib.Path(args.pop(0))
        else:
            sys.exit(__doc__)
    text = generate()
    if check:
        if not out.exists() or out.read_text() != text:
            fail(f"{out.relative_to(ROOT) if out.is_relative_to(ROOT) else out} is out of date with {HEADER.relative_to(ROOT)}: run `make lp-tokens`")
        print(f"gen-lp-tokens: {out.name} matches lp_tokens.h ({text.count('LPTokenInfo(') } tokens)")
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"gen-lp-tokens: wrote {out} ({text.count('LPTokenInfo(')} tokens)")


if __name__ == "__main__":
    main(sys.argv[1:])
