# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the DMR receive audio diagnostics (tools/plus_dmraudio.py), run by tools/test_classic.py after its own
(the build tree prepared, ASan / UBSan flags given):

- tests/plus/test_dmraudio_pcm.c: the real generated PCM driver (audio_pcm.c + pcm_playback.inc + pcm_diag.inc) and its
  audio task against a model of the prompt FIFO: the shipped feeding runs dry after a fast first block, the prefill
  does not, the counters see what the model hears, the idle stream's drops are not counted as losses;
- tests/plus/test_dmraudio_gain.c: the decoded voice's digital gain in the generated ambeVoice.c (x1 unchanged,
  saturation and its counter, the peak after it, the beeps mixed after it);
- tests/plus/test_ui_dmraudio.c: Tools > Diagnostics in upstream's simulator, English and Turkish, with the real line
  formatter and digit handler cut out of the generated boards/dm32/diag_dm32.c."""
import re
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def cut(text, begin, end):
    """The text from begin up to and including end (both must be there, once from begin)"""
    i = text.index(begin)
    j = text.index(end, i) + len(end)
    return text[i:j]


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    # The driver: the timer assignment read from the generated platform.h, as tools/test_receive_audio.py does
    platform = (APP / 'platform/hrc7000/platform.h').read_text()
    timer = re.search(r'^#define TIMER_PCM[^\n]+', platform, re.M)
    assert timer is not None
    (OUT / 'dmraudio_timer.inc').write_text(timer.group(0) + '\n')
    run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
         '-Isrc/p25', '-Itests/audio/stubs', '-I' + str(APP / 'core'), '-I' + str(APP / 'hal'),
         '-I' + str(APP / 'drivers'), '-I' + str(APP / 'platform/hrc7000'),
         '-DCONFIG_DMR=1', '-DCONFIG_P25=1', '-DCONFIG_VOX=0', '-DCONFIG_BEEP=0', '-DCONFIG_DMR_AUDIO_DIAG=1',
         '-DPCM_TIMER_INC="' + str(OUT / 'dmraudio_timer.inc') + '"',
         '-DAUDIO_PCM_C="' + str(APP / 'platform/hrc7000/audio_pcm.c') + '"',
         tp / 'test_dmraudio_pcm.c', 'src/p25/pcm_clock.c', '-o', OUT / 'test_dmraudio_pcm'], cwd=ROOT)
    run([OUT / 'test_dmraudio_pcm'], cwd=ROOT)

    # The decoded voice's gain: the generated ambeVoice.c with the real FEC
    run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
         '-I' + str(APP / 'core'), '-I' + str(APP / 'hal'), '-DCONFIG_DMR_AUDIO_DIAG=1',
         tp / 'test_dmraudio_gain.c', APP / 'core/dmr/ambe/ambeVoice.c', APP / 'core/dmr/ambe/ambeFec.c',
         '-o', OUT / 'test_dmraudio_gain'], cwd=ROOT)
    run([OUT / 'test_dmraudio_gain'], cwd=ROOT)

    # The page: the formatter and the digit handler as generated, the types from the generated header
    diag = (APP / 'boards/dm32/diag_dm32.c').read_text()
    lines = (cut(diag, 'typedef struct {\n\tchar *p, *end;\n} out_t;\n', '\nstatic void put_x(') [:-len('\nstatic void put_x(')]
             + '\n' + cut(diag, '#if CONFIG_DMR_AUDIO_DIAG && CONFIG_DMR && CONFIG_DIAG\n// DDM32 Plus DMR receive audio',
                           '\n\treturn false;\n}\n#endif\n'))
    (OUT / 'dmraudio_lines.inc').write_text(lines)
    header = (APP / 'platform/hrc7000/audio_pcm.h').read_text()
    (OUT / 'dmraudio_types.inc').write_text(
        cut(header, '#define HRC_PCM_PREFILL', '} hrc_pcm_diag_t;\n'))
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_dmraudio.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_dmraudio')
    run([*argv, '-I' + str(host), '-DCONFIG_DMR_AUDIO_DIAG=1', '-DCONFIG_DIAG=1', '-DCONFIG_LANG_TR=1',
         '-DCONFIG_P25_CHANNELS=1',
         '-DDMRAUDIO_LINES_INC="' + str(OUT / 'dmraudio_lines.inc') + '"',
         '-DDMRAUDIO_TYPES_INC="' + str(OUT / 'dmraudio_types.inc') + '"'], cwd=host)
    run([OUT / 'test_ui_dmraudio'], cwd=host)
