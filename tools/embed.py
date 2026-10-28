#!/usr/bin/env python3
"""embed.py NAME FILE OUT -- write FILE as `static const unsigned char NAME[]` to OUT."""
import sys

name, src, out = sys.argv[1:4]
data = open(src, 'rb').read()
with open(out, 'w') as f:
    f.write('static const unsigned char %s[] = {\n' % name)
    for i in range(0, len(data), 24):
        f.write(','.join(str(b) for b in data[i:i + 24]) + ',\n')
    f.write('0};\nstatic const unsigned int %s_len = %d;\n' % (name, len(data)))
