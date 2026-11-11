#!/usr/bin/env python3
"""Write the default re6gyro.ini embedded in src/config.c to the given path."""
import re, sys
s = open(sys.argv[1]).read()
start = s.index('k_default_ini[] =') + len('k_default_ini[] =')
body = s[start:s.index(';\n', start)]
txt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', body)).encode().decode('unicode_escape')
open(sys.argv[2], 'w', newline='').write(txt)
