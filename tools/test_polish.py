# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 16 polish (tools/plus_polish.py), run by tools/test_classic.py: the Turkish catalogue covers
every text the radio can show (core's .c, .h and .inc files) and each translation can be drawn."""
import importlib.util


def catalogue_tests(APP):
    """tools/i18n.py's own extraction (with the .inc files, tools/plus_polish.py) against core/ui/lang/tr.po: no text
    missing, none untranslated or fuzzy, each translation drawable (Latin-5 bytes, the alternates, spaces, colons)"""
    spec = importlib.util.spec_from_file_location('plus_i18n', APP / 'tools/i18n.py')
    i18n = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(i18n)
    assert any(f.endswith('.inc') for f in i18n.source_files()), 'tools/i18n.py does not read the .inc files'
    source = i18n.extract()
    _, entries = i18n.read_po(str(APP / 'core/ui/lang/tr.po'))
    tr = {m: (s, f) for m, s, f in entries}
    problems = []
    for en, files in source:
        if en not in tr:
            problems.append('%r (%s): not in tr.po' % (en, ', '.join(files)))
            continue
        s, fuzzy = tr[en]
        if not s or fuzzy:
            problems.append('%r (%s): not translated%s' % (en, ', '.join(files), ' (fuzzy)' if fuzzy else ''))
            continue
        problems += i18n.check_translation(en, s, 'tr.po %r' % en)
    for p in problems[:40]:
        print('FAIL catalogue:', p)
    assert not problems, '%d Turkish catalogue problem(s)' % len(problems)
    for en in ('Ring ', 'Talk on ', 'Stop, stay on ', 'In Zones', 'P25 call', ' h '):
        assert en in dict(source), en
    print('  Turkish catalogue: %d texts, every one translated and drawable (.inc files included)' % len(source))


def main(ROOT, APP, OUT, flags, host):
    catalogue_tests(APP)
