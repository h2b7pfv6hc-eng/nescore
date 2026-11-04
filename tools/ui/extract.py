# Pulls the menu drawing code out of wii/source/main.c so it can be
# rendered on a PC (tools/ui/preview.c includes the result).
import re, sys
src = open(sys.argv[1]).read()
out = []

def block(start, end):
    a = src.index(start); b = src.index(end, a)
    out.append(src[a:b])

def func(sig):
    a = src.index(sig)
    i = src.index("{", a); depth = 0
    for j in range(i, len(src)):
        if src[j] == "{": depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                out.append(src[a:j + 1] + "\n"); return

block("#define RGB(r, g, b)", "static void m_show(void)")
block("typedef struct { short x, y, w, h, id, kind; } hit_t;", "static int hit_at(")
out.append(re.search(r"typedef struct \{ char name\[128\]; int is_dir; \} entry_t;", src).group(0) + "\n")
out.append("static entry_t *entries;\nstatic int n_entries;\n")
for e in re.findall(r"^enum \{ (?:B_BACK|M_RESUME)[^;]*;", src, re.M):
    out.append(e + "\n")
for f in ["static void header(", "static void button(", "static void draw_browser(",
          "static void m_thumbnail(", "static void draw_game_menu("]:
    func(f)
open(sys.argv[2], "w").write("\n".join(out))
