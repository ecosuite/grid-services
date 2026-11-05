"""The two sides the tests play: `Sep2Client` is a CSIP-AUS client under
/d/{client}/, and `ControlClient` the utility, through the control API.

Every 2030.5 document the server sends is validated against IEEE 2030.5-2018
with the CSIP-AUS v1.1 extensions when the schema is at hand: sep.xsd from
the repository's gitignored specs/ (or CSIPAUS_SEP_XSD), merged with
vendor/csipaussd's CSIP-AUS schemas as the worker's bindings are. IEEE does
not allow sep.xsd to be redistributed, so where it is missing, as in the
image and CI, documents go unvalidated and test_schema_is_checked skips.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from lxml import etree

REPO = Path(__file__).resolve().parents[3]
SCHEMA_DIR = REPO / "vendor/csipaussd/csipaussd/schema"
SEP = "urn:ieee:std:2030.5:ns"
CSIPAUS = "https://csipaus.org/ns"
NS = {"s": SEP, "c": CSIPAUS}
TOKEN = "test-token"


def load_schema() -> etree.XMLSchema | None:
    sep_xsd = Path(os.environ.get("CSIPAUS_SEP_XSD")
                   or REPO / "specs/ieee-2030.5-2018/downloads/sep.xsd")
    if not sep_xsd.is_file() or not (SCHEMA_DIR / "merge_redefines.py").is_file():
        return None
    work = Path(tempfile.mkdtemp(prefix="csipaus-schema-"))
    try:
        subprocess.run([sys.executable, str(SCHEMA_DIR / "merge_redefines.py"), str(sep_xsd),
                        str(SCHEMA_DIR / "csipaus-core.xsd"), str(work / "sep.xsd")],
                       check=True, capture_output=True)
        shutil.copy(SCHEMA_DIR / "csipaus-ext.xsd", work)
        return etree.XMLSchema(etree.parse(str(work / "sep.xsd")))
    finally:
        shutil.rmtree(work)


SCHEMA = load_schema()


class Sep2Client:
    """A CSIP-AUS client calling as `name`."""

    def __init__(self, http, name: str):
        self.http = http
        self.name = name

    def path(self, href: str) -> str:
        """Hrefs are absolute paths; a bare name is under /d/{name}/."""
        return href if href.startswith("/") else f"/d/{self.name}/{href}"

    async def get(self, href: str, status: int = 200) -> etree._Element | None:
        response = await self.http.get(self.path(href))
        body = await response.read()
        assert response.status == status, f"GET {href}: {response.status} {body!r}"
        if status != 200:
            return None
        assert response.content_type == "application/sep+xml"
        return self.check(body)

    async def send(self, method: str, href: str, xml: str, status: int = 201) -> str | None:
        """Sends a document and returns the Location it was given, if any."""
        response = await self.http.request(
            method, self.path(href), data=xml.encode(),
            headers={"Content-Type": "application/sep+xml"})
        body = await response.read()
        assert response.status == status, f"{method} {href}: {response.status} {body!r}"
        return response.headers.get("Location")

    async def post(self, href: str, xml: str, status: int = 201) -> str | None:
        return await self.send("POST", href, xml, status)

    async def put(self, href: str, xml: str, status: int = 204) -> str | None:
        return await self.send("PUT", href, xml, status)

    @staticmethod
    def check(body: bytes) -> etree._Element:
        document = etree.fromstring(body)
        if SCHEMA is not None and not SCHEMA.validate(document):
            raise AssertionError(f"{etree.QName(document).localname} does not validate: "
                                 f"{SCHEMA.error_log.last_error}\n{body.decode()}")
        return document


class ControlClient:
    def __init__(self, http):
        self.http = http

    async def call(self, method: str, path: str, status: int | None = None,
                   **kwargs: Any) -> Any:
        response = await self.http.request(method, f"/api{path}",
                                           headers={"Authorization": f"Bearer {TOKEN}"},
                                           **kwargs)
        text = await response.text()
        if status is not None:
            assert response.status == status, f"{method} {path}: {response.status} {text}"
        elif response.status >= 400:
            raise AssertionError(f"{method} {path}: {response.status} {text}")
        return await response.json() if text and response.content_type.endswith("json") \
            else None


LFDI = "3E4F45AB31EDFE5B67E343E5E4562E31984E23E5"
SFDI = 167261211391


def end_device(lfdi: str = LFDI, sfdi: int = SFDI, changed_time: int = 1700000000) -> str:
    return (f'<EndDevice xmlns="{SEP}"><lFDI>{lfdi}</lFDI><sFDI>{sfdi}</sFDI>'
            f"<changedTime>{changed_time}</changedTime></EndDevice>")



def text(element: etree._Element, path: str) -> str | None:
    found = element.find(path, NS)
    return found.text if found is not None else None
