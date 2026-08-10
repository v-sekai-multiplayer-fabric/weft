#!/usr/bin/env python3
"""Turn a .sigs file into a C header of prototypes.

generate_stubs.py emits the dispatch table, and it emits no prototypes: Chromium's
callers include the real library headers. weft has none on the include path, on purpose.
So the prototypes come from the same .sigs file that produced the table.

One source of truth matters here more than the few lines it saves. A prototype that
disagrees with the table is a wrong call through a function pointer, which is a crash
with no diagnostic.

Usage: sigs_to_header.py <in.sigs> <out.h> <guard> <decls-header>
"""

import sys


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write(__doc__)
        return 2

    sigs_path, out_path, guard, decls = sys.argv[1:]

    with open(sigs_path, encoding="utf-8") as handle:
        lines = [
            line.strip()
            for line in handle
            if line.strip() and not line.lstrip().startswith("#")
        ]

    body = "\n".join(lines)
    out = f"""/* Generated from {sigs_path.split("/")[-1]}. Do not edit.
 *
 * The prototypes for every symbol in the dlsym dispatch table. Both come from the same
 * signature file, so a prototype cannot disagree with the table it calls through.
 */
#ifndef {guard}
#define {guard}

#include "{decls}"

#ifdef __cplusplus
extern "C" {{
#endif

{body}

#ifdef __cplusplus
}} /* extern "C" */
#endif

#endif /* {guard} */
"""

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
