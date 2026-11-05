#!/usr/bin/env python3
"""Applies CSIP-AUS's redefinitions of IEEE 2030.5 types to sep.xsd.

csipaus-core.xsd extends DERControlBase, DERSettings, DERCapability and
EndDevice through xs:redefine of sep.xsd, while csipaus-ext.xsd imports
sep.xsd. The XSD compiler refuses a schema that is both imported and
redefined, so this writes a sep.xsd with the extensions already in place:
each redefined type's added elements are appended to the end of the
original type's sequence, which is where xs:extension puts them.

The edit is textual so that the schema's namespace prefixes, which its
QName-valued attributes depend on, are left exactly as written.

    merge_redefines.py SEP_XSD CSIPAUS_CORE_XSD OUTPUT_XSD
"""

import re
import sys

CSIPAUS_NS = "https://csipaus.org/ns"
EXT_SCHEMA = "csipaus-ext.xsd"


def main(sep_path, core_path, out_path):
    with open(sep_path, encoding="utf-8-sig") as f:
        sep = f.read()
    with open(core_path, encoding="utf-8-sig") as f:
        core = f.read()

    redefine = re.search(r"<xs:redefine[^>]*>(.*)</xs:redefine>", core, re.S)
    if not redefine:
        sys.exit(f"{core_path}: no xs:redefine")

    extension = re.compile(
        r'<xs:complexType name="(\w+)">\s*<xs:complexContent>\s*'
        r'<xs:extension base="\1">\s*<xs:sequence>(.*?)</xs:sequence>',
        re.S)
    merged = []
    for match in extension.finditer(redefine.group(1)):
        name, added = match.group(1), match.group(2)
        original = re.search(
            r'(<xs:complexType name="%s">.*?)</xs:sequence>' % name, sep, re.S)
        if not original:
            sys.exit(f"{sep_path}: no complexType {name} to extend")
        sep = sep[:original.end(1)] + added + sep[original.end(1):]
        merged.append(name)

    redefined = re.findall(r'<xs:complexType name="(\w+)">',
                           redefine.group(1))
    if sorted(merged) != sorted(redefined):
        sys.exit(f"merged {merged} but {core_path} redefines {redefined}")

    root = re.search(r"<xs:schema\b[^>]*>", sep)
    opening = root.group(0).replace(
        "<xs:schema", f'<xs:schema xmlns:csipaus="{CSIPAUS_NS}"', 1)
    sep = (sep[:root.start()] + opening +
           f'\n  <xs:import namespace="{CSIPAUS_NS}" '
           f'schemaLocation="{EXT_SCHEMA}"/>' + sep[root.end():])

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(sep)
    print(f"{out_path}: extended {', '.join(merged)}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
