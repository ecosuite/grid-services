"""What ecosuite's API writes to give the site its CSIP-AUS connection,
logged in as the ecosuite_api role and nothing more.

In one transaction, as the API must: the device's client credential, the
connection and its settings, the telemetry points, the DER with its
connected and operating state bound to the inverter's, the readings the
points are posted as, and the SolarNetwork token they are read with,
secrets sealed for the worker; then a register command.
"""

from __future__ import annotations

import base64
import json
import uuid
from dataclasses import dataclass
from pathlib import Path

import psycopg
from nacl.public import PublicKey, SealedBox

from site_sim.scenario import Scenario
from site_sim.solarnode import CONNECTED, OPERATING, POINTS, STATE_POINTS
from test_common.certificates import DeviceCertificate, fingerprint

SITE = "cactus"
SITE_ID = uuid.uuid5(uuid.NAMESPACE_URL, f"grid-services-cactus-site:{SITE}")
DCAP_URL = "https://cactus-proxy/envoy/dcap"


@dataclass(frozen=True)
class Provisioner:
    database_url: str
    worker_public_key: str
    certificates: Path
    # Where the worker reaches this simulator's SolarNetwork.
    solarnetwork_host: str

    def seal(self, plaintext: str) -> str:
        """libsodium crypto_box_seal with the worker's public key, as
        standard base64, as the API seals secrets."""
        box = SealedBox(PublicKey(base64.b64decode(self.worker_public_key)))
        return base64.b64encode(box.encrypt(plaintext.encode())).decode()

    async def provision(self, scenario: Scenario, token: str, secret: str) -> int:
        """Writes the site's connection and asks the worker to register it.
        Returns the connection's id."""
        device = DeviceCertificate.load(self.certificates / "device.pem",
                                        self.certificates / "device.key")
        proxy_ca_pem = (self.certificates / "proxy-ca.pem").read_text()
        async with await psycopg.AsyncConnection.connect(self.database_url) as db:
            async with db.transaction():
                credential_id = await returning_id(
                    db, "INSERT INTO client_credentials (name, cert_pem, key_sealed, "
                    "cert_fingerprint, cert_expires_at) VALUES (%s, %s, %s, %s, %s) "
                    "RETURNING id",
                    "CACTUS device", device.cert_pem, self.seal(device.key_pem),
                    fingerprint(device.cert_pem), device.expires_at)
                connection_id = await returning_id(
                    db, "INSERT INTO connections (site_code, site_id, protocol, "
                    "client_credential_id) VALUES (%s, %s, 'csipaus', %s) RETURNING id",
                    SITE, SITE_ID, credential_id)
                await db.execute(
                    "INSERT INTO csipaus_connections (connection_id, dcap_url, ca_cert_pem, "
                    "verify_server_certificate, connection_point_id, registration_pin) "
                    "VALUES (%s, %s, %s, true, %s, %s)",
                    (connection_id, DCAP_URL, proxy_ca_pem, scenario.nmi, scenario.pin))
                # The points first: the DER and the readings refer to them.
                for point in [*POINTS, *(STATE_POINTS if scenario.der else [])]:
                    await db.execute(
                        "INSERT INTO telemetry_points (connection_id, point_id, kind, units, "
                        "source) VALUES (%s, %s, %s, %s, %s::jsonb)",
                        (connection_id, point.point_id, point.kind, point.units,
                         json.dumps(point.source())))
                if scenario.der:
                    der = {**scenario.der, "connected_point_id": CONNECTED.point_id,
                           "operating_point_id": OPERATING.point_id}
                    await db.execute(
                        f"INSERT INTO csipaus_der (connection_id, {', '.join(der)}) "
                        f"VALUES (%s, {', '.join(['%s'] * len(der))})",
                        (connection_id, *der.values()))
                for point in POINTS:
                    await db.execute(
                        "INSERT INTO csipaus_readings (connection_id, mirror, reading, "
                        "point_id) VALUES (%s, %s, %s, %s)",
                        (connection_id, point.mirror, point.reading, point.point_id))
                await db.execute(
                    "INSERT INTO connection_data_credentials (connection_id, host, token, "
                    "secret_sealed) VALUES (%s, %s, %s, %s)",
                    (connection_id, self.solarnetwork_host, token, self.seal(secret)))
                await db.execute(
                    "INSERT INTO connection_commands (connection_id, kind, requested_by) "
                    "VALUES (%s, 'register', 'site-sim')", (connection_id,))
        return connection_id


async def returning_id(db: psycopg.AsyncConnection, sql: str, *params: object) -> int:
    cursor = await db.execute(sql, params)
    row = await cursor.fetchone()
    assert row is not None
    return row[0]
