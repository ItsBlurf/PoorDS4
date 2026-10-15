# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract named production C functions for compiled host controls."""
import re

def function(text, name):
    # Production definitions put the name at column zero. Exclude indented
    # calls inside conditions as well as prototypes preceding the definition.
    match = re.search(r'^' + re.escape(name) + r'\([^;{}]*\)\s*\{', text, re.MULTILINE)
    if not match:
        raise ValueError('missing function definition: ' + name)
    pos = match.start()
    brace = match.end() - 1
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[pos:end]
