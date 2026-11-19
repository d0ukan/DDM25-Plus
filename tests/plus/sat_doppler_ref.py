#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus satellites: the radio's Doppler against a reference in Python on the same tle.txt (docs/plus.md).

    sat_doppler_ref.py PROGRAM     run PROGRAM (tests/plus/test_sat_doppler.c built) and check every line it prints

The reference is written here on its own, in plain Python (no packages): SGP4's near-earth branch from Vallado,
Crawford, Hujsak and Kelso, "Revisiting Spacetrack Report #3" (AIAA 2006-6753), WGS-72, checked first against that
paper's verification output for 00005, 06251 and 28057; the TLE read straight from integration/radiofoundry/sat/tle.txt
(the catalog numbers of sat_freq.txt); the observer on the WGS-84 ellipsoid turned into SGP4's TEME frame by the
sidereal angle (the radio does the opposite: the satellite into the earth-fixed frame), its velocity the earth's turn;
the range rate (r_sat - r_obs) . (v_sat - v_obs) / range there, and checked once more by differencing the range over a
second. Doppler: downlink -f rate / c, uplink f rate / (c - rate).

Asserted: at 145.8 MHz the radio's correction within 100 Hz of the reference (and each satellite's own downlink and
uplink within the same share), the elevation within 0.05 degree. The worst errors are printed."""
import datetime
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SAT = os.path.join(ROOT, 'integration', 'radiofoundry', 'sat')
C_KMS = 299792.458
TWOPI = 2.0 * math.pi
DEG = math.pi / 180.0

# --- SGP4, near-earth, WGS-72 -------------------------------------------------------------------------------------
MU = 398600.8
RE = 6378.135
XKE = 60.0 / math.sqrt(RE ** 3 / MU)
J2 = 0.001082616
J3 = -0.00000253881
J4 = -0.00000165597
J3OJ2 = J3 / J2


class Sgp4:
    """Vallado et al.'s sgp4init (initl included) and sgp4 for near-earth element sets; angles in radians, mean
    motion in radians a minute"""

    def __init__(self, no_kozai, ecco, inclo, nodeo, argpo, mo, bstar):
        x2o3 = 2.0 / 3.0
        self.ecco, self.inclo, self.nodeo, self.argpo, self.mo, self.bstar = ecco, inclo, nodeo, argpo, mo, bstar
        # initl
        eccsq = ecco * ecco
        omeosq = 1.0 - eccsq
        rteosq = math.sqrt(omeosq)
        cosio = math.cos(inclo)
        cosio2 = cosio * cosio
        ak = (XKE / no_kozai) ** x2o3
        d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq)
        dl = d1 / (ak * ak)
        adel = ak * (1.0 - dl * dl - dl * (1.0 / 3.0 + 134.0 * dl * dl / 81.0))
        dl = d1 / (adel * adel)
        no = no_kozai / (1.0 + dl)
        if TWOPI / no >= 225.0:
            raise ValueError('deep space')
        self.no = no
        ao = (XKE / no) ** x2o3
        sinio = math.sin(inclo)
        po = ao * omeosq
        con42 = 1.0 - 5.0 * cosio2
        self.con41 = -con42 - cosio2 - cosio2
        posq = po * po
        rp = ao * (1.0 - ecco)
        # sgp4init
        ss = 78.0 / RE + 1.0
        qzms2t = ((120.0 - 78.0) / RE) ** 4
        self.isimp = rp < (220.0 / RE + 1.0)
        sfour = ss
        qzms24 = qzms2t
        perige = (rp - 1.0) * RE
        if perige < 156.0:
            sfour = perige - 78.0
            if perige < 98.0:
                sfour = 20.0
            qzms24 = ((120.0 - sfour) / RE) ** 4
            sfour = sfour / RE + 1.0
        pinvsq = 1.0 / posq
        tsi = 1.0 / (ao - sfour)
        self.eta = eta = ao * ecco * tsi
        etasq = eta * eta
        eeta = ecco * eta
        psisq = abs(1.0 - etasq)
        coef = qzms24 * tsi ** 4
        coef1 = coef / psisq ** 3.5
        cc2 = coef1 * no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                            0.375 * J2 * tsi / psisq * self.con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)))
        self.cc1 = cc1 = bstar * cc2
        cc3 = -2.0 * coef * tsi * J3OJ2 * no * sinio / ecco if ecco > 1.0e-4 else 0.0
        self.x1mth2 = 1.0 - cosio2
        self.cc4 = 2.0 * no * coef1 * ao * omeosq * (
            eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) - J2 * tsi / (ao * psisq) *
            (-3.0 * self.con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
             0.75 * self.x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * math.cos(2.0 * argpo)))
        self.cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq)
        cosio4 = cosio2 * cosio2
        temp1 = 1.5 * J2 * pinvsq * no
        temp2 = 0.5 * temp1 * J2 * pinvsq
        temp3 = -0.46875 * J4 * pinvsq * pinvsq * no
        self.mdot = no + 0.5 * temp1 * rteosq * self.con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4)
        self.argpdot = (-0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                        temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4))
        xhdot1 = -temp1 * cosio
        self.nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio
        self.omgcof = bstar * cc3 * math.cos(argpo)
        self.xmcof = -x2o3 * coef * bstar / eeta if ecco > 1.0e-4 else 0.0
        self.nodecf = 3.5 * omeosq * xhdot1 * cc1
        self.t2cof = 1.5 * cc1
        den = 1.0 + cosio if abs(cosio + 1.0) > 1.5e-12 else 1.5e-12
        self.xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / den
        self.aycof = -0.5 * J3OJ2 * sinio
        self.delmo = (1.0 + eta * math.cos(mo)) ** 3
        self.sinmao = math.sin(mo)
        self.x7thm1 = 7.0 * cosio2 - 1.0
        self.d2 = self.d3 = self.d4 = self.t3cof = self.t4cof = self.t5cof = 0.0
        if not self.isimp:
            cc1sq = cc1 * cc1
            self.d2 = 4.0 * ao * tsi * cc1sq
            temp = self.d2 * tsi * cc1 / 3.0
            self.d3 = (17.0 * ao + sfour) * temp
            self.d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * cc1
            self.t3cof = self.d2 + 2.0 * cc1sq
            self.t4cof = 0.25 * (3.0 * self.d3 + cc1 * (12.0 * self.d2 + 10.0 * cc1sq))
            self.t5cof = 0.2 * (3.0 * self.d4 + 12.0 * cc1 * self.d3 + 6.0 * self.d2 * self.d2 +
                                15.0 * cc1sq * (2.0 * self.d2 + cc1sq))

    def run(self, t):
        """Position (km) and velocity (km/s) in TEME, t minutes from the epoch"""
        x2o3 = 2.0 / 3.0
        xmdf = self.mo + self.mdot * t
        argpdf = self.argpo + self.argpdot * t
        nodedf = self.nodeo + self.nodedot * t
        argpm = argpdf
        mm = xmdf
        t2 = t * t
        nodem = nodedf + self.nodecf * t2
        tempa = 1.0 - self.cc1 * t
        tempe = self.bstar * self.cc4 * t
        templ = self.t2cof * t2
        if not self.isimp:
            delomg = self.omgcof * t
            delm = self.xmcof * ((1.0 + self.eta * math.cos(xmdf)) ** 3 - self.delmo)
            temp = delomg + delm
            mm = xmdf + temp
            argpm = argpdf - temp
            t3 = t2 * t
            t4 = t3 * t
            tempa = tempa - self.d2 * t2 - self.d3 * t3 - self.d4 * t4
            tempe = tempe + self.bstar * self.cc5 * (math.sin(mm) - self.sinmao)
            templ = templ + self.t3cof * t3 + t4 * (self.t4cof + t * self.t5cof)
        am = (XKE / self.no) ** x2o3 * tempa * tempa
        nm = XKE / am ** 1.5
        em = self.ecco - tempe
        if em >= 1.0 or em < -0.001:
            raise ValueError('eccentricity')
        em = max(em, 1.0e-6)
        mm = mm + self.no * templ
        xlm = mm + argpm + nodem
        nodem = math.fmod(nodem, TWOPI)
        argpm = math.fmod(argpm, TWOPI)
        xlm = math.fmod(xlm, TWOPI)
        mm = math.fmod(xlm - argpm - nodem, TWOPI)
        sinip, cosip = math.sin(self.inclo), math.cos(self.inclo)
        axnl = em * math.cos(argpm)
        temp = 1.0 / (am * (1.0 - em * em))
        aynl = em * math.sin(argpm) + temp * self.aycof
        xl = mm + argpm + nodem + temp * self.xlcof * axnl
        u = math.fmod(xl - nodem, TWOPI)
        eo1 = u
        tem5 = 9999.9
        k = 1
        sineo1 = coseo1 = 0.0
        while abs(tem5) >= 1.0e-12 and k <= 10:
            sineo1 = math.sin(eo1)
            coseo1 = math.cos(eo1)
            tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl
            tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5
            if abs(tem5) >= 0.95:
                tem5 = 0.95 if tem5 > 0.0 else -0.95
            eo1 += tem5
            k += 1
        ecose = axnl * coseo1 + aynl * sineo1
        esine = axnl * sineo1 - aynl * coseo1
        el2 = axnl * axnl + aynl * aynl
        pl = am * (1.0 - el2)
        if pl < 0.0:
            raise ValueError('semi-latus rectum')
        rl = am * (1.0 - ecose)
        rdotl = math.sqrt(am) * esine / rl
        rvdotl = math.sqrt(pl) / rl
        betal = math.sqrt(1.0 - el2)
        temp = esine / (1.0 + betal)
        sinu = am / rl * (sineo1 - aynl - axnl * temp)
        cosu = am / rl * (coseo1 - axnl + aynl * temp)
        su = math.atan2(sinu, cosu)
        sin2u = (cosu + cosu) * sinu
        cos2u = 1.0 - 2.0 * sinu * sinu
        temp = 1.0 / pl
        temp1 = 0.5 * J2 * temp
        temp2 = temp1 * temp
        mrt = rl * (1.0 - 1.5 * temp2 * betal * self.con41) + 0.5 * temp1 * self.x1mth2 * cos2u
        su = su - 0.25 * temp2 * self.x7thm1 * sin2u
        xnode = nodem + 1.5 * temp2 * cosip * sin2u
        xinc = self.inclo + 1.5 * temp2 * cosip * sinip * cos2u
        mvt = rdotl - nm * temp1 * self.x1mth2 * sin2u / XKE
        rvdot = rvdotl + nm * temp1 * (self.x1mth2 * cos2u + 1.5 * self.con41) / XKE
        sinsu, cossu = math.sin(su), math.cos(su)
        snod, cnod = math.sin(xnode), math.cos(xnode)
        sini, cosi = math.sin(xinc), math.cos(xinc)
        xmx, xmy = -snod * cosi, cnod * cosi
        ux, uy, uz = xmx * sinsu + cnod * cossu, xmy * sinsu + snod * cossu, sini * sinsu
        vx, vy, vz = xmx * cossu - cnod * sinsu, xmy * cossu - snod * sinsu, sini * cossu
        vk = RE * XKE / 60.0
        r = (mrt * ux * RE, mrt * uy * RE, mrt * uz * RE)
        v = ((mvt * ux + rvdot * vx) * vk, (mvt * uy + rvdot * vy) * vk, (mvt * uz + rvdot * vz) * vk)
        return r, v


# --- TLE ------------------------------------------------------------------------------------------------------------
def implied(s):
    """'-12345-6' -> -0.12345e-6"""
    s = s.strip()
    sign = -1.0 if s.startswith('-') else 1.0
    s = s.lstrip('+-')
    mant, exp = s[:-2], s[-2:]
    return sign * float('0.' + mant) * 10.0 ** int(exp)


def tle(l1, l2):
    """(catalog number, epoch in seconds since 2000-01-01 UTC, Sgp4)"""
    yy = int(l1[18:20])
    year = 2000 + yy if yy < 57 else 1900 + yy
    epoch = datetime.datetime(year, 1, 1) + datetime.timedelta(days=float(l1[20:32]) - 1.0)
    epoch_s = (epoch - datetime.datetime(2000, 1, 1)).total_seconds()
    sat = Sgp4(no_kozai=float(l2[52:63]) * TWOPI / 1440.0, ecco=float('0.' + l2[26:33].strip()),
               inclo=float(l2[8:16]) * DEG, nodeo=float(l2[17:25]) * DEG, argpo=float(l2[34:42]) * DEG,
               mo=float(l2[43:51]) * DEG, bstar=implied(l1[53:61]))
    return int(l1[2:7]), epoch_s, sat


def shipped():
    """{catalog number: (epoch_s, Sgp4)} for the satellites sat_freq.txt names, from tle.txt"""
    want = set()
    with open(os.path.join(SAT, 'sat_freq.txt'), encoding='ascii') as f:
        for line in f:
            if line.strip() and not line.startswith('#'):
                want.add(int(line.split()[0]))
    lines = [l.rstrip('\n') for l in open(os.path.join(SAT, 'tle.txt'), encoding='ascii') if not l.startswith('#')]
    out = {}
    for i, l in enumerate(lines):
        if l.startswith('1 ') and i + 1 < len(lines) and lines[i + 1].startswith('2 '):
            num, epoch_s, sat = tle(l, lines[i + 1])
            if num in want:
                out[num] = (epoch_s, sat)
    return out


def self_check():
    """The verification output of Vallado et al. 2006 (tcppver.out, WGS-72) for three near-earth sets"""
    cases = [
        ('1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753',
         '2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667',
         [(0.0, (7022.46529266, -1400.08296755, 0.03995155), (1.893841015, 6.405893759, 4.534807250)),
          (360.0, (-7154.03120202, -3783.17682504, -3536.19412294), (4.741887409, -4.151817765, -2.093935425)),
          (720.0, (-7134.59340119, 6531.68641334, 3260.27186483), (-4.113793027, -2.911922039, -2.557327851)),
          (1080.0, (5568.53901181, 4492.06992591, 3863.87641983), (-4.209106476, 5.159719888, 2.744852980)),
          (1440.0, (-938.55923943, -6268.18748831, -4294.02924751), (7.536105209, -0.427127707, 0.989878080))]),
        ('1 06251U 62025E   06176.82412014  .00008885  00000-0  12808-3 0  3985',
         '2 06251  58.0579  54.0425 0030035 139.1568 221.1854 15.56387291  6374',
         [(0.0, (3988.31022699, 5498.96657235, 0.90055879), (-3.290032738, 2.357652820, 6.496623475)),
          (120.0, (-3935.69800083, 409.10980837, 5471.33577327), (-3.374784183, -6.635211043, -1.942056221))]),
        ('1 28057U 03049A   06177.78615833  .00000060  00000-0  35940-4 0  1836',
         '2 28057  98.4283 247.6961 0000884  88.1964 271.9322 14.35478080140550',
         [(0.0, (-2715.28237486, -6619.26436889, -0.01341443), (-1.008587273, 0.422782003, 7.385272942))]),
    ]
    worst_r = worst_v = 0.0
    for l1, l2, rows in cases:
        _, _, sat = tle(l1, l2)
        for t, rw, vw in rows:
            r, v = sat.run(t)
            worst_r = max(worst_r, max(abs(a - b) for a, b in zip(r, rw)))
            worst_v = max(worst_v, max(abs(a - b) for a, b in zip(v, vw)))
    print(f'  reference SGP4 against Vallado et al.: worst {worst_r:.1e} km, {worst_v:.1e} km/s')
    if worst_r > 1e-4 or worst_v > 1e-7:
        raise SystemExit('sat_doppler_ref: the reference SGP4 does not reproduce the verification output')


# --- Frames ---------------------------------------------------------------------------------------------------------
OMEGA_E = 7.292115e-5


def gmst(s2000):
    """Vallado's gstime (IAU 1982) from the Julian date (UT1 ~ UTC), radians"""
    jd = 2451544.5 + s2000 / 86400.0
    tut1 = (jd - 2451545.0) / 36525.0
    g = (-6.2e-6 * tut1 ** 3 + 0.093104 * tut1 ** 2 + (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841)
    g = math.fmod(g * DEG / 240.0, TWOPI)
    return g + TWOPI if g < 0.0 else g


def observer_teme(lat_e7, lon_e7, alt_m, s2000):
    """The observer's position and velocity (km, km/s) in TEME, and its zenith there"""
    a, f = 6378.137, 1.0 / 298.257223563
    e2 = f * (2.0 - f)
    lat, lon, h = lat_e7 * 1e-7 * DEG, lon_e7 * 1e-7 * DEG, alt_m / 1000.0
    n = a / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
    x, y, z = (n + h) * math.cos(lat) * math.cos(lon), (n + h) * math.cos(lat) * math.sin(lon), (n * (1.0 - e2) + h) * math.sin(lat)
    th = gmst(s2000)
    c, s = math.cos(th), math.sin(th)
    r = (c * x - s * y, s * x + c * y, z)
    v = (-OMEGA_E * r[1], OMEGA_E * r[0], 0.0)
    zen = (math.cos(lat) * math.cos(lon + th), math.cos(lat) * math.sin(lon + th), math.sin(lat))
    return r, v, zen


def look(sat, epoch_s, obs, t):
    """(elevation in degrees, range km, range rate km/s) at t seconds since 2000"""
    rs, vs = sat.run((t - epoch_s) / 60.0)
    ro, vo, zen = observer_teme(*obs, t)
    d = [a - b for a, b in zip(rs, ro)]
    dv = [a - b for a, b in zip(vs, vo)]
    rng = math.sqrt(sum(x * x for x in d))
    rate = sum(a * b for a, b in zip(d, dv)) / rng
    el = math.asin(sum(a * b for a, b in zip(d, zen)) / rng) / DEG
    return el, rng, rate


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    self_check()
    sats = shipped()
    out = subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout.splitlines()
    obs = tuple(int(x) for x in out[0].split()[1:4])
    worst = {'145': 0.0, 'down': 0.0, 'up': 0.0, 'el': 0.0, 'diff': 0.0}
    n = 0
    per = {}
    bad = []
    for line in out[1:]:
        f = line.split()
        if f[0] != 'D':
            raise SystemExit(f'sat_doppler_ref: the program says {line!r}')
        num, t, el_c, rate_c = int(f[1]), int(f[2]), float(f[3]), float(f[4])
        d145, ddown, dup, down, up = (int(x) for x in f[5:10])
        epoch_s, sat = sats[num]
        el, rng, rate = look(sat, epoch_s, obs, t)
        # the range rate once more: the range's change over a second
        _, r1, _ = look(sat, epoch_s, obs, t - 0.5)
        _, r2, _ = look(sat, epoch_s, obs, t + 0.5)
        worst['diff'] = max(worst['diff'], abs((r2 - r1) - rate))
        ref145 = -145.8e6 * rate / C_KMS
        refdown = -down * rate / C_KMS
        refup = up * rate / (C_KMS - rate)
        e145, edown, eup = abs(d145 - ref145), abs(ddown - refdown), abs(dup - refup)
        worst['145'] = max(worst['145'], e145)
        worst['down'] = max(worst['down'], edown * 145.8e6 / down)
        worst['up'] = max(worst['up'], eup * 145.8e6 / up)
        worst['el'] = max(worst['el'], abs(el_c - el))
        per[num] = per.get(num, 0) + 1
        n += 1
        if e145 >= 100.0 or edown * 145.8e6 / down >= 100.0 or eup * 145.8e6 / up >= 100.0 or abs(el_c - el) >= 0.05:
            bad.append(f'{num} t={t}: el {el_c:.3f}/{el:.3f}, 145.8 MHz {d145}/{ref145:.1f} Hz, down {ddown}/{refdown:.1f}, up {dup}/{refup:.1f}')
    print(f'  {n} samples ({", ".join(f"{k}: {v}" for k, v in sorted(per.items()))}) near or above the horizon over Ankara')
    print(f'  Doppler at 145.8 MHz: worst {worst["145"]:.2f} Hz off the reference (limit 100 Hz); the satellites\' own '
          f'downlink {worst["down"]:.2f} Hz, uplink {worst["up"]:.2f} Hz (scaled to 145.8 MHz)')
    print(f'  elevation worst {worst["el"]:.4f} deg off; the reference range rate against its range\'s change over a '
          f'second: {worst["diff"] * 1000:.2f} m/s')
    if len(per) != len(sats) or n < 200:
        raise SystemExit('sat_doppler_ref: too few samples, or a satellite missing')
    if worst['diff'] > 0.005:
        raise SystemExit('sat_doppler_ref: the reference range rate is not the range\'s derivative')
    if bad:
        raise SystemExit('sat_doppler_ref: off the reference:\n' + '\n'.join(bad[:20]))
    print('sat_doppler_ref: ok')


if __name__ == '__main__':
    main()
