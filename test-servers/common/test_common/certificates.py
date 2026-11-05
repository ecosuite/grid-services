"""IEEE 2030.5 device certificates for tests.

A CSIP-AUS server knows a device by its certificate: the LFDI is the
certificate's SHA-256 fingerprint, over its DER encoding, left-truncated to
160 bits, and the SFDI is the fingerprint left-truncated to 36 bits, in
decimal with a sum-of-digits check digit appended. The worker derives both
the same way (src/protocols/csipaus/identity.cpp).

Certificates are ECDSA P-256 and self-signed: the curve IEEE 2030.5's
cipher suite needs, and enough for a test server, which does not check the
issuer.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def fingerprint(cert_pem: str) -> str:
    """The SHA-256 of the certificate's DER encoding, as upper-case hex."""
    certificate = x509.load_pem_x509_certificate(cert_pem.encode())
    der = certificate.public_bytes(serialization.Encoding.DER)
    return hashlib.sha256(der).hexdigest().upper()


def lfdi(cert_pem: str) -> str:
    """The certificate's LFDI: 40 upper-case hex digits."""
    return fingerprint(cert_pem)[:40]


def sfdi(lfdi_hex: str) -> int:
    """The SFDI for an LFDI."""
    value = int(lfdi_hex[:9], 16)
    digit_sum = sum(int(digit) for digit in str(value))
    return value * 10 + (10 - digit_sum % 10) % 10


@dataclass(frozen=True)
class DeviceCertificate:
    cert_pem: str
    key_pem: str

    @property
    def lfdi(self) -> str:
        return lfdi(self.cert_pem)

    @property
    def sfdi(self) -> int:
        return sfdi(self.lfdi)

    @property
    def expires_at(self) -> datetime:
        certificate = x509.load_pem_x509_certificate(self.cert_pem.encode())
        return certificate.not_valid_after_utc

    @classmethod
    def generate(cls, common_name: str, days: int = 3650) -> DeviceCertificate:
        key = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
        now = datetime.now(timezone.utc)
        certificate = (
            x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(minutes=5))
            .not_valid_after(now + timedelta(days=days))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .sign(key, hashes.SHA256())
        )
        return cls(
            cert_pem=certificate.public_bytes(serialization.Encoding.PEM).decode(),
            key_pem=key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            ).decode(),
        )

    @classmethod
    def load(cls, cert_path: Path, key_path: Path) -> DeviceCertificate:
        return cls(cert_pem=cert_path.read_text(), key_pem=key_path.read_text())

    def save(self, cert_path: Path, key_path: Path) -> None:
        cert_path.parent.mkdir(parents=True, exist_ok=True)
        cert_path.write_text(self.cert_pem)
        key_path.touch(mode=0o600)
        key_path.write_text(self.key_pem)
