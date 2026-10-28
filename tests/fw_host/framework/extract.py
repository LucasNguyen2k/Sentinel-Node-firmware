#!/usr/bin/env python3
"""
extract.py - copy selected top-level items (functions, types, globals, #defines) verbatim out of
firmware sources so host unit tests always exercise the shipped code, never a hand-made copy.

The firmware is an Arduino sketch: one large .ino whose functions cannot be compiled on a PC as a
whole (they depend on the Realtek SDK). The test declares which symbols it needs; this tool
finds each symbol's complete top-level definition, keeps the surrounding #if/#ifdef context,
emits the items in their original file order and adds prototypes for the extracted functions
(the Arduino builder normally generates those).

Selector syntax (whitespace separated):
    name            every top-level item that defines `name` (function, struct/enum, variable, #define)
    name@text       only items named `name` whose header contains `text` (pick one overload),
                    e.g.  getSensorData@getSensorData()
    Class::method   an out-of-class method definition

Usage:  extract.py SOURCE OUT.inc SELECTOR...
"""
import re
import sys

IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


def split_items(src):
    """Split a C/C++ file into top-level items. Returns a list of dicts:
       {text, kind, names, header, cond}  (cond = preprocessor condition stack, list of frames)."""
    items = []
    cond = []          # list of frames; frame = list of directive lines ("#ifdef X", "#else", ...)
    i, n = 0, len(src)
    start = 0
    depth = 0
    paren = 0

    def at_line_start(pos):
        j = pos - 1
        while j >= 0 and src[j] in " \t":
            j -= 1
        return j < 0 or src[j] == "\n"

    def push(text):
        t = text.strip()
        if t:
            items.append(classify(t, [list(f) for f in cond]))

    while i < n:
        c = src[i]
        # comments
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # string / char literals
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        # preprocessor directive at depth 0
        if c == "#" and depth == 0 and at_line_start(i):
            # flush anything pending before the directive (normally whitespace/comments)
            pending = src[start:i]
            if pending.strip() and not pending.strip().startswith("//"):
                # a declaration interrupted by a directive is unusual; keep it as its own item
                push(pending)
            j = i
            while True:
                e = src.find("\n", j)
                if e < 0:
                    e = n
                    break
                if src[e - 1] == "\\":
                    j = e + 1
                    continue
                break
            line = src[i:e]
            d = line.split()[0] if line.split() else "#"
            word = re.sub(r"^#\s*", "#", d)
            if word in ("#if", "#ifdef", "#ifndef"):
                cond.append([line.strip()])
            elif word in ("#elif", "#else"):
                if cond:
                    cond[-1].append(line.strip())
            elif word == "#endif":
                if cond:
                    cond.pop()
            else:
                items.append(classify(line.strip(), [list(f) for f in cond]))
            i = e
            start = e
            continue
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                # end of a function body / struct / initializer; include a trailing ';' if present
                j = i + 1
                while j < n and src[j] in " \t\r\n":
                    j += 1
                if j < n and src[j] == ";":
                    i = j
                push(src[start:i + 1])
                start = i + 1
        elif c == ";" and depth == 0 and paren == 0:
            push(src[start:i + 1])
            start = i + 1
        i += 1
    if src[start:].strip():
        push(src[start:])
    return items


def strip_comments(t):
    t = re.sub(r"//[^\n]*", "", t)
    return re.sub(r"/\*.*?\*/", "", t, flags=re.S)


def classify(text, cond):
    t = strip_comments(text).strip()
    item = {"text": text, "cond": cond, "names": [], "kind": "other", "header": ""}
    if t.startswith("#"):
        m = re.match(r"#\s*define\s+(" + IDENT + ")", t)
        if m:
            item.update(kind="define", names=[m.group(1)], header=t.split("\n")[0])
        else:
            item.update(kind="directive", header=t)
        return item
    brace = t.find("{")
    head = t if brace < 0 else t[:brace]
    item["header"] = " ".join(head.split())
    m = re.match(r"(?:typedef\s+)?(struct|enum|class|union)\s+(" + IDENT + r")\s*(?::[^{]*)?$", head.strip())
    if m and brace >= 0:
        item.update(kind="type", names=[m.group(2)])
        return item
    if t.startswith("typedef"):
        m = re.search(r"(" + IDENT + r")\s*(?:\[[^\]]*\])?\s*;\s*$", t)
        item.update(kind="type", names=[m.group(1)] if m else [])
        return item
    if t.startswith('extern "C"'):
        item.update(kind="extern")
        return item
    # function definition: header has '(' and body follows, no '=' before the parameter list
    if brace >= 0 and "(" in head and "=" not in head.split("(")[0]:
        m = re.search(r"((?:" + IDENT + r"::)?~?" + IDENT + r")\s*\(", head)
        if m:
            item.update(kind="function", names=[m.group(1)])
            return item
    # prototype
    if brace < 0 and "(" in t and "=" not in t.split("(")[0]:
        m = re.search(r"(" + IDENT + r")\s*\(", t)
        if m:
            item.update(kind="prototype", names=[m.group(1)])
            return item
    # variable declaration(s): split declarators at depth 0
    decl = t.rstrip(";")
    parts, cur, d = [], "", 0
    for ch in decl:
        if ch in "({[":
            d += 1
        elif ch in ")}]":
            d -= 1
        if ch == "," and d == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    names = []
    for p in parts:
        p = p.split("=")[0]
        p = re.sub(r"\[[^\]]*\]", "", p)
        m = re.search(r"(" + IDENT + r")\s*$", p.strip())
        if m:
            names.append(m.group(1))
    item.update(kind="variable", names=names)
    return item


def wrap(item, body):
    out = []
    for frame in item["cond"]:
        out.extend(frame)
    out.append(body)
    out.extend(["#endif"] * len(item["cond"]))
    return "\n".join(out)


def prototype(item):
    h = item["header"].strip()
    if "::" in item["names"][0]:
        return None
    return h + ";"


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    src_path, out_path, selectors = sys.argv[1], sys.argv[2], sys.argv[3:]
    src = open(src_path, encoding="utf-8").read()
    items = split_items(src)
    chosen = set()
    missing = []
    for sel in selectors:
        name, _, must = sel.partition("@")
        hits = [k for k, it in enumerate(items) if name in it["names"] and (not must or must in it["header"])]
        if not hits:
            missing.append(sel)
        chosen.update(hits)
    if missing:
        sys.stderr.write("extract.py: not found in %s: %s\n" % (src_path, " ".join(missing)))
        sys.exit(1)
    order = sorted(chosen)
    protos = [prototype(items[k]) for k in order if items[k]["kind"] == "function"]
    out = ["// ---- generated by extract.py from %s - do not edit ----" % src_path]
    # Prototypes go before the first function, but after the last extracted type definition, so that
    # every type a prototype mentions is already declared.
    first_fn = next((i for i, k in enumerate(order) if items[k]["kind"] == "function"), None)
    last_type = max((i for i, k in enumerate(order) if items[k]["kind"] in ("type", "define")), default=-1)
    proto_at = None if first_fn is None else (first_fn if first_fn > last_type else last_type + 1)
    for idx, k in enumerate(order):
        if idx == proto_at:
            out.append("// prototypes (normally generated by the Arduino builder)")
            for k2 in order:
                if items[k2]["kind"] == "function":
                    p = prototype(items[k2])
                    if p:
                        out.append(wrap(items[k2], p))
        out.append(wrap(items[k], items[k]["text"]))
    open(out_path, "w", encoding="utf-8").write("\n\n".join(out) + "\n")


if __name__ == "__main__":
    main()
