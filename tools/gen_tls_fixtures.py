#!/usr/bin/env python3
"""Create the certificates of the boot case pkg_https (docs/design/tls.md).

    tools/gen_tls_fixtures.py

The script writes a test CA with an RSA key to user/etc/tests/tls-ca.pem,
from where the build installs it in /etc/tests, and a server certificate
with a P-256 key to tests/cases/pkg_https/server.pem and server.key. The
server certificate is valid for the IPv4 address 10.0.2.2, under which
the guest reaches the host, and for the name repo.test. Both
certificates are valid from 2026-01-01 to 2126-01-01. The keys protect
nothing but the test and are part of the repository."""
import datetime
import ipaddress
import os

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
END = datetime.datetime(2126, 1, 1, tzinfo=datetime.timezone.utc)


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def builder(subject, issuer, key):
    return (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(name(issuer))
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(START).not_valid_after(END))


def usage(sign, cert_sign):
    return x509.KeyUsage(digital_signature=sign, content_commitment=False, key_encipherment=False,
                         data_encipherment=False, key_agreement=False, key_cert_sign=cert_sign, crl_sign=cert_sign,
                         encipher_only=False, decipher_only=False)


ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ca = (builder("minios test CA", "minios test CA", ca_key)
      .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
      .add_extension(usage(False, True), critical=True)
      .sign(ca_key, hashes.SHA256()))
server_key = ec.generate_private_key(ec.SECP256R1())
server = (builder("repo.test", "minios test CA", server_key)
          .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
          .add_extension(usage(True, False), critical=True)
          .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
          .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("10.0.2.2")),
                                                      x509.DNSName("repo.test")]), critical=False)
          .sign(ca_key, hashes.SHA256()))

with open(os.path.join(TOP, "user/etc/tests/tls-ca.pem"), "wb") as f:
    f.write(ca.public_bytes(serialization.Encoding.PEM))
case = os.path.join(TOP, "tests/cases/pkg_https")
with open(os.path.join(case, "server.pem"), "wb") as f:
    f.write(server.public_bytes(serialization.Encoding.PEM))
with open(os.path.join(case, "server.key"), "wb") as f:
    f.write(server_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption()))
