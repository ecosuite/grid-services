"""NMIs: a connection point's 10-character National Metering Identifier
and the checksum AEMO's NMI Procedure (Appendix 1) adds as an 11th
character, as the CSIP-AUS explainer (10.1) asks a connection point to be
given."""

from __future__ import annotations


def checksum(nmi: str) -> int:
    """From the right, every other character's ASCII code doubled, starting
    with the last; the sum of the digits of all of them, up to the next
    multiple of ten."""
    if len(nmi) != 10 or not nmi.isascii() or not nmi.isalnum():
        raise ValueError(f"an NMI is 10 letters and digits, not {nmi!r}")
    codes = [ord(character) * (2 if index % 2 == 0 else 1)
             for index, character in enumerate(reversed(nmi.upper()))]
    total = sum(int(digit) for code in codes for digit in str(code))
    return (10 - total % 10) % 10


def with_checksum(nmi: str) -> str:
    return nmi.upper() + str(checksum(nmi))
