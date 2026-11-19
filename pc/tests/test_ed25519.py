"""radiotools/ed25519.py against the RFC 8032 section 7.1 test vectors, and the signed manifest checks."""
import json
import unittest

import rtfixtures as fx
from radiotools import ed25519
from radiotools import signed_manifest as sm

# RFC 8032, 7.1: (secret key, public key, message, signature), TEST 1, 2, 3 and SHA(abc)
VECTORS = (
    ('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
     'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a', '',
     'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f059'
     '5bbe24655141438e7a100b'),
    ('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb',
     '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c', '72',
     '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4'
     '302aeeb00d291612bb0c00'),
    ('c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7',
     'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025', 'af82',
     '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e971'
     '6ed28dc027beceea1ec40a'),
    ('833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42',
     'ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf',
     'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643c'
     'e80e2a9ac94fa54ca49f',
     'dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138a'
     'c17ad9bef1177331a704'),
)


class Rfc8032Test(unittest.TestCase):
    def test_vectors(self):
        for sk, pk, msg, sig in VECTORS:
            sk, pk, msg, sig = map(bytes.fromhex, (sk, pk, msg, sig))
            with self.subTest(pk=pk.hex()[:8]):
                self.assertEqual(ed25519.secret_to_public(sk), pk)
                self.assertEqual(ed25519.sign(sk, msg), sig)
                self.assertTrue(ed25519.verify(pk, msg, sig))

    def test_bad_signatures(self):
        sk, pk, msg, sig = map(bytes.fromhex, VECTORS[2])
        self.assertFalse(ed25519.verify(pk, msg + b'\0', sig))
        for i in (0, 31, 32, 63):
            bad = bytearray(sig); bad[i] ^= 1
            self.assertFalse(ed25519.verify(pk, msg, bytes(bad)))
        self.assertFalse(ed25519.verify(pk, msg, sig[:63]))
        s = int.from_bytes(sig[32:], 'little') + ed25519.q          # non-canonical S (malleability) is refused
        if s < 1 << 256:
            self.assertFalse(ed25519.verify(pk, msg, sig[:32] + s.to_bytes(32, 'little')))
        other = bytes.fromhex(VECTORS[0][1])
        self.assertFalse(ed25519.verify(other, msg, sig))
        with self.assertRaises(ValueError):
            ed25519.verify(pk[:31], msg, sig)


class ManifestTest(unittest.TestCase):
    APP = fx.pattern(0x5000, 5)

    def m(self, **kw):
        return fx.manifest([fx.firmware_entry(self.APP, **kw)])

    def test_good_signature(self):
        data, sig = fx.signed(self.m())
        m = sm.verify_bytes(data, sig, fx.TEST_PUBLIC_HEX)
        self.assertEqual(sm.firmware_for(m, sm.sha256(self.APP))['version'], '1.0.0')
        self.assertIsNone(sm.firmware_for(m, '0' * 64))

    def test_bad_signatures(self):
        data, sig = fx.signed(self.m())
        cases = {
            'other key': (data, fx.signed(self.m(), fx.OTHER_SECRET)[1]),
            'changed byte': (data.replace(b'"stable"', b'"beta"'), sig),
            'appended': (data + b' ', sig),
            'truncated sig': (data, sig.strip()[:-2]),
            'not hex': (data, 'zz' * 64),
            'flipped sig': (data, ('0' if sig[0] != '0' else '1') + sig[1:]),
        }
        for name, (d, s) in cases.items():
            with self.subTest(case=name), self.assertRaises(sm.ManifestError):
                sm.verify_bytes(d, s, fx.TEST_PUBLIC_HEX)

    def test_no_key_embedded_verifies_nothing(self):
        data, sig = fx.signed(self.m())
        self.assertEqual(sm.PUBLIC_KEY_HEX, '', 'the public key is pasted in by the maintainer after --keygen')
        with self.assertRaisesRegex(sm.ManifestError, 'No release signing key'):
            sm.verify_bytes(data, sig)
        with self.assertRaisesRegex(sm.ManifestError, 'malformed'):
            sm.verify_bytes(data, sig, 'abc')

    def test_schema_refusals_even_when_signed(self):
        base = self.m()
        bad = []
        for field, value in (('channel', 'nightly'), ('url', 'https://example.com/x.zip'),
                             ('url', fx.url('v1.0.0', '../evil.zip')), ('file', '../x.zip'), ('sha256', 'ab'),
                             ('settings_format', 'one'), ('settings_format', True), ('min_compatible_version', 'x'),
                             ('application_size', 0xE8000), ('version', '1.0'), ('notes', {'tr': 'only'})):
            m = json.loads(json.dumps(base))
            m['firmware'][0][field] = value
            bad.append((field, m))
        m = json.loads(json.dumps(base)); m['repo'] = 'someone/else'; bad.append(('repo', m))
        m = json.loads(json.dumps(base)); m['format'] = 'x'; bad.append(('format', m))
        m = json.loads(json.dumps(base)); m['firmware'].append(dict(m['firmware'][0])); bad.append(('dup', m))
        for name, m in bad:
            data, sig = fx.signed(m)
            with self.subTest(field=name), self.assertRaises(sm.ManifestError):
                sm.verify_bytes(data, sig, fx.TEST_PUBLIC_HEX)

    def test_versions_sort(self):
        order = ['0.9.0', '1.0.0-beta.1', '1.0.0-beta.2', '1.0.0-beta.10', '1.0.0-rc.1', '1.0.0', '1.0.1', '1.10.0']
        self.assertEqual(sorted(reversed(order), key=sm.parse_version), order)
        with self.assertRaises(sm.ManifestError):
            sm.parse_version('v1.0.0')


if __name__ == '__main__':
    unittest.main()
