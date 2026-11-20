# SPDX-License-Identifier: GPL-3.0-or-later
"""The TLS context for every download of the program.

Python on Windows trusts the roots that are in the Windows store at that moment. Windows adds missing roots only when
its own network stack asks for them, so on some PCs a GitHub release download fails with "unable to get issuer
certificate" although a browser opens the same address. The context below trusts the Windows store plus the Mozilla
roots of certifi when it is present (bundled with the program). TLS is not what makes a release trusted: every
firmware and program file is checked against the Ed25519-signed manifest before it is used.
"""
import ssl

_context = None


def ca_bundle():
    """certifi's CA file, or None when certifi is not available"""
    try:
        import certifi
        return certifi.where()
    except (ImportError, OSError):
        return None


def context():
    """A verifying TLS context (hostname and certificate checks on), built once"""
    global _context
    if _context is None:
        ctx = ssl.create_default_context()
        path = ca_bundle()
        if path:
            try:
                ctx.load_verify_locations(cafile=path)
            except (OSError, ssl.SSLError):
                pass
        _context = ctx
    return _context
