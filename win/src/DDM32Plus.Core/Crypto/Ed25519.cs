// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using System.Security.Cryptography;

namespace DDM32Plus.Core.Crypto;

/// <summary>
/// Ed25519 (RFC 8032, PureEdDSA): a line-by-line port of pc/radiotools/ed25519.py, which is the reference code of
/// RFC 8032 section 6. Same changes from the RFC as the Python file: field values are reduced mod p before the zero
/// test in RecoverX, a wrong signature length returns false, and the helpers take and return bytes.
///
/// Verify() is what the program uses (public data only, timing does not matter). Sign() is for the tests only; it is
/// not constant-time. Tested against the RFC 8032 section 7.1 vectors and against signatures made by the Python code.
/// </summary>
public static class Ed25519
{
    // Base field Z_p
    internal static readonly BigInteger P = BigInteger.Pow(2, 255) - 19;

    // Curve constant d = -121665 / 121666
    internal static readonly BigInteger D = Mod(-121665 * ModpInv(121666));

    // Group order
    public static readonly BigInteger Q = BigInteger.Pow(2, 252) + BigInteger.Parse("27742317777372353535851937790883648493");

    // Square root of -1
    private static readonly BigInteger ModpSqrtM1 = BigInteger.ModPow(2, (P - 1) / 4, P);

    // Base point
    private static readonly BigInteger GY = Mod(4 * ModpInv(5));
    private static readonly BigInteger GX = RecoverX(GY, 0) ?? throw new InvalidOperationException("base point");
    private static readonly Point G = new(GX, GY, 1, Mod(GX * GY));

    /// <summary>A point in extended coordinates (X, Y, Z, T): x = X/Z, y = Y/Z, x*y = T/Z.</summary>
    private readonly record struct Point(BigInteger X, BigInteger Y, BigInteger Z, BigInteger T);

    private static BigInteger Mod(BigInteger x)
    {
        var r = BigInteger.Remainder(x, P);
        return r.Sign < 0 ? r + P : r;
    }

    private static BigInteger ModpInv(BigInteger x) => BigInteger.ModPow(Mod(x), P - 2, P);

    private static BigInteger FromLittle(ReadOnlySpan<byte> bytes) => new(bytes, isUnsigned: true, isBigEndian: false);

    private static byte[] ToLittle32(BigInteger v)
    {
        var out32 = new byte[32];
        if (!v.TryWriteBytes(out32, out _, isUnsigned: true, isBigEndian: false))
            throw new ArgumentOutOfRangeException(nameof(v));
        return out32;
    }

    private static BigInteger Sha512ModQ(params byte[][] parts)
    {
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
        foreach (var part in parts)
            h.AppendData(part);
        var r = BigInteger.Remainder(FromLittle(h.GetHashAndReset()), Q);
        return r;
    }

    private static Point PointAdd(Point p, Point q)
    {
        var a = Mod((p.Y - p.X) * (q.Y - q.X));
        var b = Mod((p.Y + p.X) * (q.Y + q.X));
        var c = Mod(2 * p.T * q.T * D);
        var d = Mod(2 * p.Z * q.Z);
        BigInteger e = b - a, f = d - c, g = d + c, h = b + a;
        return new Point(Mod(e * f), Mod(g * h), Mod(f * g), Mod(e * h));
    }

    /// <summary>Q = s * P</summary>
    private static Point PointMul(BigInteger s, Point p)
    {
        var q = new Point(0, 1, 1, 0); // neutral element
        while (s.Sign > 0)
        {
            if (!s.IsEven)
                q = PointAdd(q, p);
            p = PointAdd(p, p);
            s >>= 1;
        }
        return q;
    }

    private static bool PointEqual(Point p, Point q)
    {
        // x1 / z1 == x2 / z2  <==>  x1 * z2 == x2 * z1
        if (Mod(p.X * q.Z - q.X * p.Z) != 0)
            return false;
        if (Mod(p.Y * q.Z - q.Y * p.Z) != 0)
            return false;
        return true;
    }

    /// <summary>The x coordinate for y with the low bit 'sign', or null.</summary>
    private static BigInteger? RecoverX(BigInteger y, int sign)
    {
        if (y >= P)
            return null;
        var x2 = Mod((y * y - 1) * ModpInv(D * y * y + 1));
        if (x2.IsZero)
            return sign != 0 ? null : BigInteger.Zero;
        // square root of x2
        var x = BigInteger.ModPow(x2, (P + 3) / 8, P);
        if (Mod(x * x - x2) != 0)
            x = Mod(x * ModpSqrtM1);
        if (Mod(x * x - x2) != 0)
            return null;
        if ((int)(x & 1) != sign)
            x = P - x;
        return x;
    }

    private static byte[] PointCompress(Point p)
    {
        var zinv = ModpInv(p.Z);
        var x = Mod(p.X * zinv);
        var y = Mod(p.Y * zinv);
        return ToLittle32(y | ((x & 1) << 255));
    }

    private static Point? PointDecompress(ReadOnlySpan<byte> s)
    {
        if (s.Length != 32)
            throw new ArgumentException("Invalid input length for decompression");
        var y = FromLittle(s);
        var sign = (int)(y >> 255);
        y &= (BigInteger.One << 255) - 1;
        var x = RecoverX(y, sign);
        if (x is null)
            return null;
        return new Point(x.Value, y, 1, Mod(x.Value * y));
    }

    /// <summary>True when the 32 bytes are a valid curve point (a usable public key).</summary>
    public static bool IsValidPoint(ReadOnlySpan<byte> publicKey) =>
        publicKey.Length == 32 && PointDecompress(publicKey) is not null;

    private static (BigInteger A, byte[] Prefix) SecretExpand(ReadOnlySpan<byte> secret)
    {
        if (secret.Length != 32)
            throw new ArgumentException("Bad size of private key");
        var h = SHA512.HashData(secret);
        var a = FromLittle(h.AsSpan(0, 32));
        a &= (BigInteger.One << 254) - 8;
        a |= BigInteger.One << 254;
        return (a, h[32..]);
    }

    public static byte[] SecretToPublic(ReadOnlySpan<byte> secret)
    {
        var (a, _) = SecretExpand(secret);
        return PointCompress(PointMul(a, G));
    }

    /// <summary>The 64-byte signature of msg (tests only; not constant-time).</summary>
    public static byte[] Sign(ReadOnlySpan<byte> secret, byte[] msg)
    {
        var (a, prefix) = SecretExpand(secret);
        var pub = PointCompress(PointMul(a, G));
        var r = Sha512ModQ(prefix, msg);
        var rs = PointCompress(PointMul(r, G));
        var h = Sha512ModQ(rs, pub, msg);
        var s = BigInteger.Remainder(r + h * a, Q);
        return [.. rs, .. ToLittle32(s)];
    }

    /// <summary>True only for a valid signature of msg by the 32-byte public key.</summary>
    public static bool Verify(byte[] publicKey, byte[] msg, byte[] signature)
    {
        if (publicKey.Length != 32)
            throw new ArgumentException("Bad public key length");
        if (signature.Length != 64)
            return false;
        var a = PointDecompress(publicKey);
        if (a is null)
            return false;
        var rs = signature[..32];
        var r = PointDecompress(rs);
        if (r is null)
            return false;
        var s = FromLittle(signature.AsSpan(32));
        if (s >= Q)
            return false;
        var h = Sha512ModQ(rs, publicKey, msg);
        var sB = PointMul(s, G);
        var hA = PointMul(h, a.Value);
        return PointEqual(sB, PointAdd(r.Value, hA));
    }
}
