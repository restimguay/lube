# Makes client.c (the page script as C strings) from client.js: `make web-client`, after changing client.js.
# The lines go without their indentation or comments, and in pieces under 4095 bytes (the longest string
# C99 promises). client.c's comment at the top stays as it is.
import os, re

here = os.path.dirname(os.path.abspath(__file__))
src, dst = os.path.join(here, 'client.js'), os.path.join(here, 'client.c')
js = open(src).read()
old = open(dst).read()
header = old[:old.index('static const char')]
lines = []
for line in js.split('\n'):
    st = line.strip()
    if not st or st.startswith('//'):
        continue
    lines.append(re.sub(r"\s+//.*$", "", line).strip())
pieces, cur, n = [], [], 0
for l in lines:
    if n + len(l) + 1 > 3500:
        pieces.append(cur)
        cur, n = [], 0
    cur.append(l)
    n += len(l) + 1
pieces.append(cur)
body = ',\n'.join('\n'.join('    "' + l.replace('\\', '\\\\').replace('"', '\\"') + '\\n"' for l in piece)
                  for piece in pieces)
open(dst, 'w').write(header + 'static const char *const CLIENT_JS[] = {\n' + body + ',\n    NULL};\n')
print('client.c: %d bytes, in %d pieces' % (sum(len(l) + 1 for l in lines), len(pieces)))
