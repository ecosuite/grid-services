"""The device certificates connect.py makes, and their identifiers."""

import stat

from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec

from test_common.certificates import DeviceCertificate, lfdi, sfdi

# The worker's test certificate (tests/support/csipaus_fixtures.hpp). The
# worker's tests check its LFDI against openssl; this checks that the two
# derivations agree.
WORKER_TEST_CERTIFICATE = """\
-----BEGIN CERTIFICATE-----
MIIBnzCCAUWgAwIBAgIUTPWClZV9ilF0VykAWdRjNQNKTSowCgYIKoZIzj0EAwIw
JDEiMCAGA1UEAwwZZ3JpZC1zZXJ2aWNlcyB0ZXN0IGRldmljZTAgFw0yNjEwMDQy
MjUzMThaGA8yMTI2MDkxMDIyNTMxOFowJDEiMCAGA1UEAwwZZ3JpZC1zZXJ2aWNl
cyB0ZXN0IGRldmljZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABHJxpo7pzraA
KLty9CStAjs0nCKWM4VYvEDABUKp2xZJH0MMX09dQnvoyyJ8JVp8EZfILU0M/pPy
/Qnfkp+KremjUzBRMB0GA1UdDgQWBBTPZnsKzXV+yPHN5A87JQ7RuIq7DDAfBgNV
HSMEGDAWgBTPZnsKzXV+yPHN5A87JQ7RuIq7DDAPBgNVHRMBAf8EBTADAQH/MAoG
CCqGSM49BAMCA0gAMEUCIG/0gZche/PAsUnf1u3DGdJOEuymWR2GYbyHOX5SvXrD
AiEAzFTjEb/ijxgzb/nh2Hs0+Gfk0VSPqriAb2uQ+oTAnkc=
-----END CERTIFICATE-----
"""


def test_sfdi_follows_the_standards_example():
    assert sfdi("3E4F45AB31EDFE5B67E343E5E4562E31984E23E5") == 167261211391


def test_lfdi_agrees_with_the_worker():
    assert lfdi(WORKER_TEST_CERTIFICATE) == "C995EBDDFCFA53DE23DA8EA2BFFF91834D51D617"
    assert sfdi(lfdi(WORKER_TEST_CERTIFICATE)) == 541127305912


def test_generates_p256_certificates_and_keeps_them(tmp_path):
    certificate = DeviceCertificate.generate("site-1")
    loaded = x509.load_pem_x509_certificate(certificate.cert_pem.encode())
    assert isinstance(loaded.public_key().curve, ec.SECP256R1)

    cert_path, key_path = tmp_path / "site-1.pem", tmp_path / "site-1.key"
    certificate.save(cert_path, key_path)
    assert stat.S_IMODE(key_path.stat().st_mode) == 0o600
    again = DeviceCertificate.load(cert_path, key_path)
    assert again.lfdi == certificate.lfdi
    assert len(again.lfdi) == 40
    assert sum(int(digit) for digit in str(again.sfdi)) % 10 == 0
