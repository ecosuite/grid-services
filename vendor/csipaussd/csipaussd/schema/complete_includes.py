#!/usr/bin/env python3
"""Adds the includes XSD 4.0.0 leaves out of file-per-type sources.

A generated header includes its base type's header and only forward-declares
the types of its own members, which its source file then includes. A derived
type's source file also needs its bases' member types complete, as its
constructors pass them to the base by std::unique_ptr, but the compiler does
not include them. This adds, to each source file, the headers of the types
forward-declared by its own header and its bases' headers.

    complete_includes.py GENERATED_DIR
"""

import os
import re
import sys

LOCAL_INCLUDE = re.compile(r'^#include "([^"]+\.hxx)"$', re.M)
FORWARD_DECLARATION = re.compile(r"^\s*class (\w+);$", re.M)


def main(directory):
    headers = {}
    for name in os.listdir(directory):
        if name.endswith(".hxx"):
            with open(os.path.join(directory, name)) as f:
                headers[name] = f.read()

    def ancestry(header, seen):
        if header in seen or header not in headers:
            return
        seen.add(header)
        for included in LOCAL_INCLUDE.findall(headers[header]):
            ancestry(included, seen)

    added = 0
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".cxx"):
            continue
        header = name[:-4] + ".hxx"
        path = os.path.join(directory, name)
        with open(path) as f:
            source = f.read()

        lineage = set()
        ancestry(header, lineage)
        present = set(LOCAL_INCLUDE.findall(source))
        missing = sorted(
            {f"{declared}.hxx"
             for ancestor in lineage
             for declared in FORWARD_DECLARATION.findall(headers[ancestor])}
            - present - {header}
            & headers.keys())
        if not missing:
            continue

        own = f'#include "{header}"\n'
        if own not in source:
            sys.exit(f"{path}: does not include {header}")
        source = source.replace(
            own, own + "".join(f'#include "{h}"\n' for h in missing), 1)
        with open(path, "w") as f:
            f.write(source)
        added += len(missing)

    print(f"{directory}: added {added} includes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
