// SPDX-License-Identifier: GPL-3.0-or-later
//! Ed25519 (RFC 8032, PureEdDSA): a line-by-line port of pc/radiotools/ed25519.py (the reference code of RFC 8032
//! section 6) and of the C# port (Crypto/Ed25519.cs). Same changes from the RFC: field values are reduced mod p
//! before the zero test in `recover_x`, a wrong signature length returns false, the helpers take and return bytes.
//!
//! Why a port and not ed25519-dalek: the program must accept and refuse exactly what the Python and C# programs do,
//! including the edge cases of the reference code (a non-canonical public key or R is refused, s >= q is refused,
//! the cofactorless equation). `verify` handles public data only, so timing does not matter; `sign` is for the tests
//! only and is not constant-time.

use num_bigint::BigInt;
use num_integer::Integer;
use num_traits::{One, Signed, Zero};
use sha2::{Digest, Sha512};
use std::sync::OnceLock;

struct Consts {
    p: BigInt,
    d: BigInt,
    q: BigInt,
    sqrt_m1: BigInt,
    g: Point,
}

#[derive(Clone)]
struct Point {
    x: BigInt,
    y: BigInt,
    z: BigInt,
    t: BigInt,
}

fn consts() -> &'static Consts {
    static C: OnceLock<Consts> = OnceLock::new();
    C.get_or_init(|| {
        let p: BigInt = (BigInt::one() << 255) - 19;
        let md = |x: BigInt| x.mod_floor(&p);
        let inv = |x: &BigInt| md(x.clone()).modpow(&(&p - 2), &p);
        let d = md(BigInt::from(-121665) * inv(&BigInt::from(121666)));
        let q: BigInt = (BigInt::one() << 252) + "27742317777372353535851937790883648493".parse::<BigInt>().unwrap();
        let sqrt_m1 = BigInt::from(2).modpow(&((&p - 1) / 4), &p);
        let gy = md(BigInt::from(4) * inv(&BigInt::from(5)));
        let mut c = Consts {
            p: p.clone(),
            d,
            q,
            sqrt_m1,
            g: Point { x: BigInt::zero(), y: BigInt::zero(), z: BigInt::one(), t: BigInt::zero() },
        };
        let gx = recover_x_with(&c, &gy, 0).expect("base point");
        c.g = Point { t: md(&gx * &gy), x: gx, y: gy, z: BigInt::one() };
        c
    })
}

fn md(x: BigInt) -> BigInt {
    x.mod_floor(&consts().p)
}

fn modp_inv(x: &BigInt) -> BigInt {
    let p = &consts().p;
    md(x.clone()).modpow(&(p - 2), p)
}

fn from_little(b: &[u8]) -> BigInt {
    BigInt::from_bytes_le(num_bigint::Sign::Plus, b)
}

fn to_little32(v: &BigInt) -> [u8; 32] {
    let (_, bytes) = v.to_bytes_le();
    assert!(bytes.len() <= 32, "value too large");
    let mut out = [0u8; 32];
    out[..bytes.len()].copy_from_slice(&bytes);
    out
}

fn sha512_mod_q(parts: &[&[u8]]) -> BigInt {
    let mut h = Sha512::new();
    for p in parts {
        h.update(p);
    }
    from_little(&h.finalize()).mod_floor(&consts().q)
}

fn point_add(p: &Point, q: &Point) -> Point {
    let d = &consts().d;
    let a = md((&p.y - &p.x) * (&q.y - &q.x));
    let b = md((&p.y + &p.x) * (&q.y + &q.x));
    let c = md(BigInt::from(2) * &p.t * &q.t * d);
    let dd = md(BigInt::from(2) * &p.z * &q.z);
    let (e, f, g, h) = (&b - &a, &dd - &c, &dd + &c, &b + &a);
    Point { x: md(&e * &f), y: md(&g * &h), z: md(&f * &g), t: md(&e * &h) }
}

/// Q = s * P
fn point_mul(s: &BigInt, p: &Point) -> Point {
    let mut s = s.clone();
    let mut p = p.clone();
    let mut q = Point { x: BigInt::zero(), y: BigInt::one(), z: BigInt::one(), t: BigInt::zero() };
    while s.is_positive() {
        if s.is_odd() {
            q = point_add(&q, &p);
        }
        p = point_add(&p, &p);
        s >>= 1;
    }
    q
}

fn point_equal(p: &Point, q: &Point) -> bool {
    md(&p.x * &q.z - &q.x * &p.z).is_zero() && md(&p.y * &q.z - &q.y * &p.z).is_zero()
}

fn recover_x_with(c: &Consts, y: &BigInt, sign: u8) -> Option<BigInt> {
    let p = &c.p;
    if y >= p {
        return None;
    }
    let mdp = |x: BigInt| x.mod_floor(p);
    let inv = |x: BigInt| mdp(x).modpow(&(p - 2), p);
    let x2 = mdp((y * y - 1) * inv(&c.d * y * y + 1));
    if x2.is_zero() {
        return if sign != 0 { None } else { Some(BigInt::zero()) };
    }
    let mut x = x2.modpow(&((p + 3) / 8), p);
    if !mdp(&x * &x - &x2).is_zero() {
        x = mdp(&x * &c.sqrt_m1);
    }
    if !mdp(&x * &x - &x2).is_zero() {
        return None;
    }
    let low = if x.is_odd() { 1 } else { 0 };
    if low != sign {
        x = p - x;
    }
    Some(x)
}

fn point_compress(p: &Point) -> [u8; 32] {
    let zinv = modp_inv(&p.z);
    let x = md(&p.x * &zinv);
    let y = md(&p.y * &zinv);
    let v = y | ((x & BigInt::one()) << 255);
    to_little32(&v)
}

fn point_decompress(s: &[u8]) -> Option<Point> {
    assert_eq!(s.len(), 32, "Invalid input length for decompression");
    let mut y = from_little(s);
    let sign = if (&y >> 255u32).is_zero() { 0 } else { 1 };
    y &= (BigInt::one() << 255) - 1;
    let x = recover_x_with(consts(), &y, sign)?;
    let t = md(&x * &y);
    Some(Point { x, y, z: BigInt::one(), t })
}

/// True when the 32 bytes are a valid curve point (a usable public key).
pub fn is_valid_point(public_key: &[u8]) -> bool {
    public_key.len() == 32 && point_decompress(public_key).is_some()
}

fn secret_expand(secret: &[u8]) -> (BigInt, Vec<u8>) {
    assert_eq!(secret.len(), 32, "Bad size of private key");
    let h = Sha512::digest(secret);
    let mut a = from_little(&h[..32]);
    a &= (BigInt::one() << 254) - 8;
    a |= BigInt::one() << 254;
    (a, h[32..].to_vec())
}

/// The public key of a 32-byte secret.
pub fn secret_to_public(secret: &[u8]) -> [u8; 32] {
    let (a, _) = secret_expand(secret);
    point_compress(&point_mul(&a, &consts().g))
}

/// The 64-byte signature of msg (tests and tools only; not constant-time).
pub fn sign(secret: &[u8], msg: &[u8]) -> [u8; 64] {
    let c = consts();
    let (a, prefix) = secret_expand(secret);
    let public = point_compress(&point_mul(&a, &c.g));
    let r = sha512_mod_q(&[&prefix, msg]);
    let rs = point_compress(&point_mul(&r, &c.g));
    let h = sha512_mod_q(&[&rs, &public, msg]);
    let s = (r + h * a).mod_floor(&c.q);
    let mut out = [0u8; 64];
    out[..32].copy_from_slice(&rs);
    out[32..].copy_from_slice(&to_little32(&s));
    out
}

/// A public key of the wrong length (the C# `ArgumentException`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BadKeyLength;

/// True only for a valid signature of msg by the 32-byte public key; a key that is not 32 bytes is an error.
pub fn verify(public_key: &[u8], msg: &[u8], signature: &[u8]) -> Result<bool, BadKeyLength> {
    if public_key.len() != 32 {
        return Err(BadKeyLength);
    }
    if signature.len() != 64 {
        return Ok(false);
    }
    let c = consts();
    let Some(a) = point_decompress(public_key) else { return Ok(false) };
    let rs = &signature[..32];
    let Some(r) = point_decompress(rs) else { return Ok(false) };
    let s = from_little(&signature[32..]);
    if s >= c.q {
        return Ok(false);
    }
    let h = sha512_mod_q(&[rs, public_key, msg]);
    let sb = point_mul(&s, &c.g);
    let ha = point_mul(&h, &a);
    Ok(point_equal(&sb, &point_add(&r, &ha)))
}
