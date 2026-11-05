"""The certificates every run uses, made once and kept in certs/, which git
ignores.

The proxy's: a CA, standing in for SERCA, and a server certificate it signs
for `cactus-proxy`, both ECDSA P-256 as IEEE 2030.5's cipher suite needs.
Connections trust the CA through their `ca_cert_pem`. And the device's: one
self-signed device certificate, whose LFDI every run's EndDevice has. Runs
do not share a server, so they can share it.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from test_common.certificates import DeviceCertificate

CERTIFICATES = Path(__file__).resolve().parent.parent / "certs"
PROXY_HOST = "cactus-proxy"


@dataclass(frozen=True)
class Certificates:
    proxy_ca_pem: str
    device: DeviceCertificate


def name(common_name: str) -> x509.Name:
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])


def pem(certificate: x509.Certificate) -> bytes:
    return certificate.public_bytes(serialization.Encoding.PEM)


def make_proxy_certificates(directory: Path) -> None:
    now = datetime.now(timezone.utc)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca = (
        x509.CertificateBuilder()
        .subject_name(name("grid-services CACTUS proxy CA"))
        .issuer_name(name("grid-services CACTUS proxy CA"))
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=5))
        .not_valid_after(now + timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(x509.KeyUsage(digital_signature=False, content_commitment=False,
                                     key_encipherment=False, data_encipherment=False,
                                     key_agreement=False, key_cert_sign=True, crl_sign=True,
                                     encipher_only=False, decipher_only=False), critical=True)
        .sign(ca_key, hashes.SHA256())
    )
    key = ec.generate_private_key(ec.SECP256R1())
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name(PROXY_HOST))
        .issuer_name(ca.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=5))
        .not_valid_after(now + timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(PROXY_HOST)]), critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "proxy-ca.pem").write_bytes(pem(ca))
    (directory / "proxy.pem").write_bytes(pem(certificate) + pem(ca))
    (directory / "proxy.key").write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))


def ensure(directory: Path = CERTIFICATES) -> Certificates:
    """The certificates, made first if they are not there yet."""
    if not all((directory / name).exists() for name in ("proxy-ca.pem", "proxy.pem", "proxy.key")):
        make_proxy_certificates(directory)
    cert_path = directory / "device.pem"
    key_path = directory / "device.key"
    if cert_path.exists() and key_path.exists():
        device = DeviceCertificate.load(cert_path, key_path)
    else:
        device = DeviceCertificate.generate("grid-services CACTUS device")
        device.save(cert_path, key_path)
    return Certificates((directory / "proxy-ca.pem").read_text(), device)
