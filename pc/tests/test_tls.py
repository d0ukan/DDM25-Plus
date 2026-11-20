# SPDX-License-Identifier: GPL-3.0-or-later
import ssl
import unittest
from unittest.mock import patch

from ddm32_app import tls


class TlsContextTest(unittest.TestCase):
    def setUp(self):
        tls._context = None

    def tearDown(self):
        tls._context = None

    def test_verifies_and_is_cached(self):
        ctx = tls.context()
        self.assertIsInstance(ctx, ssl.SSLContext)
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(ctx.check_hostname)
        self.assertIs(tls.context(), ctx)

    def test_without_certifi_still_verifies(self):
        with patch.object(tls, 'ca_bundle', return_value=None):
            ctx = tls.context()
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)

    def test_unreadable_bundle_is_ignored(self):
        with patch.object(tls, 'ca_bundle', return_value='does-not-exist.pem'):
            self.assertEqual(tls.context().verify_mode, ssl.CERT_REQUIRED)

    @unittest.skipUnless(tls.ca_bundle(), 'certifi not installed')
    def test_certifi_roots_loaded(self):
        base = ssl.create_default_context().cert_store_stats()['x509_ca']
        self.assertGreaterEqual(tls.context().cert_store_stats()['x509_ca'], base)
        self.assertGreater(tls.context().cert_store_stats()['x509_ca'], 100)


if __name__ == '__main__':
    unittest.main()
