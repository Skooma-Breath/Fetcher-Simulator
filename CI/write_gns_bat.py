#!/usr/bin/env python3
"""Write a bat file with CRLF line endings for CMD compatibility."""
import sys
vcvars = sys.argv[1]
arch = sys.argv[2]
outpath = sys.argv[3]
crlf = b'\r\n'
content = b'@call "' + vcvars.encode() + b'" ' + arch.encode() + crlf + b'@set' + crlf
open(outpath, 'wb').write(content)
