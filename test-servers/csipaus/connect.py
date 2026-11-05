"""Makes a csipaus connection to try the worker against this server by hand.

Creates a device certificate for the site, or reuses the one it made
before, and writes what the API would: the certificate as a client
credential, its key sealed for the worker, the connection and its CSIP-AUS
settings pointing at /d/<site>/dcap on this server. With --register it also
asks the worker to register.

    uv run --project test-servers/csipaus test-servers/csipaus/connect.py site-1 --register

The defaults suit `docker compose up`: the database on 127.0.0.1:5433, the
worker's development key pair, and the server as the worker reaches it,
http://test-csipaus:8080. Certificates are kept in test-servers/csipaus/
certs/, which git ignores, so a site keeps its LFDI from one run to the
next. --certificate-only stops after the certificate, for giving it to the
API instead.
"""

from __future__ import annotations

import argparse
import os
import sys
import uuid
from pathlib import Path

import psycopg

from test_common.certificates import DeviceCertificate, fingerprint
from test_common.e2e import Api

CERTIFICATES = Path(__file__).resolve().parent / "certs"
# compose.yaml's development key pair.
WORKER_PUBLIC_KEY = "4tVVc3eYLH+j0lCc6w56DWvLLZ7qIwuecQJZFB21kmg="
DATABASE_URL = "postgresql://grid_services:grid_services@127.0.0.1:5433/grid_services"
SERVER_URL = "http://test-csipaus:8080"


def certificate_for(site: str, new: bool) -> tuple[DeviceCertificate, Path, Path, bool]:
    cert_path = CERTIFICATES / f"{site}.pem"
    key_path = CERTIFICATES / f"{site}.key"
    if not new and cert_path.exists() and key_path.exists():
        return DeviceCertificate.load(cert_path, key_path), cert_path, key_path, False
    certificate = DeviceCertificate.generate(f"grid-services test device {site}")
    certificate.save(cert_path, key_path)
    return certificate, cert_path, key_path, True


def connect(api: Api, site: str, certificate: DeviceCertificate, dcap_url: str,
            connection_point_id: str | None, pin: int | None) -> int:
    """Inserts the credential, connection and settings in one transaction, as
    the API must, and returns the connection's id."""
    # The same site always gets the same id, as ecosuite's would.
    site_id = uuid.uuid5(uuid.NAMESPACE_URL, f"grid-services-test-site:{site}")
    with api.db.transaction():
        credential_id = api.one(
            "INSERT INTO client_credentials (name, cert_pem, key_sealed, "
            "cert_fingerprint, cert_expires_at) VALUES (%s, %s, %s, %s, %s) RETURNING id",
            f"{site} test device", certificate.cert_pem, api.seal(certificate.key_pem),
            fingerprint(certificate.cert_pem), certificate.expires_at)["id"]
        connection_id = api.one(
            "INSERT INTO connections (site_code, site_id, protocol, client_credential_id) "
            "VALUES (%s, %s, 'csipaus', %s) RETURNING id",
            site, site_id, credential_id)["id"]
        api.execute(
            "INSERT INTO csipaus_connections (connection_id, dcap_url, connection_point_id, "
            "registration_pin) VALUES (%s, %s, %s, %s)",
            connection_id, dcap_url, connection_point_id, pin)
    return connection_id


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("site", help="the site code, also the client name on the server")
    parser.add_argument("--register", action="store_true",
                        help="ask the worker to register the connection")
    parser.add_argument("--nmi", metavar="ID", dest="connection_point_id",
                        help="the connection point the worker sends on registration")
    parser.add_argument("--pin", type=int,
                        help="the registration PIN to check (the server's default is 111115)")
    parser.add_argument("--server", default=SERVER_URL,
                        help=f"the server as the worker reaches it (default {SERVER_URL})")
    parser.add_argument("--database-url",
                        default=os.environ.get("GRID_SERVICES_DEV_DATABASE_URL", DATABASE_URL))
    parser.add_argument("--worker-public-key",
                        default=os.environ.get("GRID_SERVICES_PUBLIC_KEY", WORKER_PUBLIC_KEY))
    parser.add_argument("--new-certificate", action="store_true",
                        help="replace the site's certificate, and so its LFDI")
    parser.add_argument("--certificate-only", action="store_true",
                        help="make the certificate and stop")
    args = parser.parse_args()

    certificate, cert_path, key_path, created = certificate_for(args.site, args.new_certificate)
    print(f"Certificate  {cert_path} ({'new' if created else 'existing'})")
    print(f"Key          {key_path}")
    print(f"LFDI         {certificate.lfdi}")
    print(f"SFDI         {certificate.sfdi}")
    if args.certificate_only:
        return 0

    dcap_url = f"{args.server.rstrip('/')}/d/{args.site}/dcap"
    api = Api(args.database_url, timeout=10, worker_public_key=args.worker_public_key)
    try:
        connection_id = connect(api, args.site, certificate, dcap_url,
                                args.connection_point_id, args.pin)
        if args.register:
            api.execute(
                "INSERT INTO connection_commands (connection_id, kind, requested_by) "
                "VALUES (%s, 'register', 'connect.py')", connection_id)
    except psycopg.errors.UniqueViolation:
        print(f"\nSite {args.site} already has a connection that is not archived. "
              "Archive it, or use another site.", file=sys.stderr)
        return 1
    finally:
        api.close()

    print(f"Connection   {connection_id}, dcap_url {dcap_url}")
    if args.register:
        print("Asked the worker to register it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
