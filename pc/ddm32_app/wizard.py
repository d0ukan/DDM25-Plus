"""The install and restore flows' rules, without Qt: which step may start when.

Install: backup -> detect -> plan -> install. Restore: choose backup -> detect -> plan -> restore.
Radio steps (backup, install, restore) start only from their own button (one click = one approved step) and each
needs the previous step to have succeeded; nothing here ever starts a radio step by itself. Detect and plan only read
files and open no port, so the UI runs them right after a verified backup. A failed install or restore is never
retried automatically: if the tool says no write was sent (receipt writes_started false, e.g. the radio was not
switched on in time) the user may plan and start again by hand; once writes had started, the install stays locked
until a new verified backup exists (a restore may always be planned again: it writes everything from the backup).
"""

IDLE, RUNNING, OK, FAILED = 'idle', 'running', 'ok', 'failed'
MODES = {'over_classic': 'install_plus', 'from_stock': 'install_plus_from_stock'}


class Wizard:
    def __init__(self):
        self.port = None
        self.package = None          # releases.Package
        self.backup = IDLE
        self.backup_dir = None
        self.backup_existing = False
        self.detect = IDLE
        self.detection = None        # radio_detect's JSON
        self.plan = IDLE
        self.plan_data = None
        self.plan_key = None         # (backup_dir, package sha, mode) the plan was made for
        self.install = IDLE
        self.install_result = None
        self.locked = False
        self.message = ''
        self.running = None

    # --- inputs -------------------------------------------------------------------------------------------------
    def set_port(self, port):
        if self.running in ('backup', 'install'):
            raise RuntimeError('port change while a radio step runs')
        self.port = port or None

    def set_package(self, package):
        if self.running:
            raise RuntimeError('package change while a step runs')
        if package is not self.package:
            self.package = package
            self._reset_plan()

    def _reset_plan(self):
        self.plan, self.plan_data, self.plan_key = IDLE, None, None
        if self.install != RUNNING:
            self.install = IDLE

    @property
    def mode(self):
        """install_plus / install_plus_from_stock for the detected radio, or None"""
        d = self.detection
        if self.detect != OK or not d or not d.get('supported'):
            return None
        return MODES.get(d.get('install_mode'))

    # --- enabled buttons ----------------------------------------------------------------------------------------
    def can_backup(self):
        return self.running is None and bool(self.port)

    def can_detect(self):
        return self.running is None and self.backup == OK and not self.locked

    def can_plan(self):
        return (self.running is None and self.backup == OK and self.detect == OK and self.mode is not None and
                not self.locked and self.install != OK and self.package is not None and self.package.installable)

    def can_install(self):
        return (self.running is None and self.plan == OK and bool(self.port) and not self.locked and
                self.install == IDLE and self.package is not None and self.package.installable and
                self.plan_key == (str(self.backup_dir), self.package.sha256, self.mode))

    def buttons(self):
        return {'backup': self.can_backup(), 'plan': self.can_plan(), 'install': self.can_install()}

    # --- backup -------------------------------------------------------------------------------------------------
    def start_backup(self, backup_dir):
        if not self.can_backup():
            raise RuntimeError('backup not allowed now')
        self.running = 'backup'
        self.backup, self.backup_dir, self.backup_existing = RUNNING, backup_dir, False
        self.detect, self.detection = IDLE, None
        self._reset_plan()
        self.install, self.install_result = IDLE, None
        self.message = 'Yedek alınıyor: araç portu açınca telsizi açın.'

    def backup_finished(self, result):
        self.running = None
        if result.verified:
            self.backup = OK
            self.locked = False
            self.message = 'Yedek doğrulandı (VERIFIED).'
        else:
            self.backup = FAILED
            self.message = ('Telsiz zamanında açılmadı. Telsizi kapatın, "Tekrar dene"ye basın, sonra açın.'
                            if result.timeout else 'Yedek alınamadı: ' + result.error)

    def use_existing_backup(self, backup_dir):
        """A verified backup already on disk (the installer checks again that the radio still matches it)"""
        if self.running is not None:
            raise RuntimeError('busy')
        self.backup, self.backup_dir, self.backup_existing = OK, backup_dir, True
        self.detect, self.detection = IDLE, None
        self.locked = False
        self._reset_plan()
        self.install, self.install_result = IDLE, None
        self.message = 'Mevcut yedek seçildi.'

    # --- detect (no port) ---------------------------------------------------------------------------------------
    def start_detect(self):
        if not self.can_detect():
            raise RuntimeError('detect not allowed now')
        self.running = 'detect'
        self.detect, self.detection = RUNNING, None
        self._reset_plan()

    def detect_finished(self, detection, error=''):
        self.running = None
        if detection is not None:
            self.detect, self.detection = OK, detection
            self.message = '' if detection.get('supported') else (detection.get('reason') or 'Desteklenmiyor')
        else:
            self.detect, self.detection = FAILED, None
            self.message = 'Telsizdeki yazılım anlaşılamadı: ' + (error or 'bilinmeyen hata')

    # --- plan (no port) -----------------------------------------------------------------------------------------
    def start_plan(self):
        if not self.can_plan():
            raise RuntimeError('plan not allowed now')
        self.running = 'plan'
        self.plan, self.plan_data = RUNNING, None
        self.plan_key = (str(self.backup_dir), self.package.sha256, self.mode)
        self.message = 'Plan hesaplanıyor (port açılmaz).'

    def plan_finished(self, plan, error='', safe=True):
        self.running = None
        if plan is not None and safe:
            self.plan, self.plan_data = OK, plan
            self.install = IDLE
            self.message = 'Plan hazır.'
        else:
            self.plan, self.plan_data, self.plan_key = FAILED, plan, None
            self.message = 'Plan yapılamadı: ' + (error or 'güvenli olmayan plan')

    # --- install ------------------------------------------------------------------------------------------------
    def start_install(self):
        if not self.can_install():
            raise RuntimeError('install not allowed now')
        self.running = 'install'
        self.install = RUNNING
        self.plan = 'used'           # one plan, one install click: a new try needs a new plan
        self.message = 'Kurulum sürüyor: bitene kadar kabloyu çekmeyin, telsizi kapatmayın, pencereyi kapatmayın.'

    def install_finished(self, result):
        self.running = None
        self.install_result = result
        if result.passed:
            self.install = OK
            self.backup = 'stale'    # the radio no longer matches the backup
            self.message = 'Kurulum tamam: VERIFIED, PASS.'
        else:
            self.install = FAILED
            if result.writes_started:
                self.locked = True
                self.message = ('Kurulum yazma başladıktan sonra durdu: otomatik tekrar yok. "Geri dön" sayfasından '
                                'yedeğinizle geri yükleyebilirsiniz.')
            else:
                self.install = IDLE
                self.plan = IDLE
                self.message = ('Kurulum başlamadı, hiçbir şey yazılmadı' +
                                (' (telsiz zamanında açılmadı)' if result.timeout else '') +
                                '. İsterseniz Kontrol ve Yükle adımlarını elle yeniden yapın.')


class RestoreFlow:
    """Geri dön: a chosen backup of this radio -> detect -> plan -> restore. The tool follows the backup's layout:
    Classic / DDM32 Plus layout -> install_plus.py --restore; stock -> install_firstboot.py --restore-stock"""

    def __init__(self):
        self.port = None
        self.kind = None             # 'previous' or 'stock'
        self.backup_dir = None
        self.detect, self.detection = IDLE, None
        self.plan, self.plan_data = IDLE, None
        self.restore, self.result = IDLE, None
        self.running = None
        self.message = ''

    def choose(self, kind, backup_dir):
        if self.running:
            raise RuntimeError('busy')
        self.kind, self.backup_dir = kind, backup_dir
        self.detect, self.detection = IDLE, None
        self.plan, self.plan_data = IDLE, None
        self.restore, self.result = IDLE, None
        self.message = ''

    @property
    def tool(self):
        """(tool, extra args, plan mode) for the chosen backup, or None when it may not be restored this way"""
        d = self.detection
        if self.detect != OK or not d:
            return None
        if d.get('layout') == 'stock':
            return 'install_firstboot', ['--restore-stock', '--use-saved-backup'], 'restore_stock'
        if d.get('layout') == 'classic' and self.kind == 'previous':
            return 'install_plus', ['--restore'], 'restore'
        return None

    def can_detect(self):
        return self.running is None and self.backup_dir is not None

    def start_detect(self):
        if not self.can_detect():
            raise RuntimeError('detect not allowed now')
        self.running, self.detect = 'detect', RUNNING

    def detect_finished(self, detection, error=''):
        self.running = None
        self.detect, self.detection = (OK, detection) if detection is not None else (FAILED, None)
        if detection is None:
            self.message = 'Yedekteki yazılım anlaşılamadı: ' + (error or 'bilinmeyen hata')
        elif self.tool is None:
            self.message = ('Bu yedek stok yazılım değil: "Stok yazılıma dön" yalnızca stok yazılımın yedeğiyle '
                            'yapılır.' if self.kind == 'stock' else 'Bu yedek geri yüklenemez: ' +
                            (detection.get('reason') or 'tanınmayan yazılım'))

    def can_plan(self):
        return self.running is None and self.tool is not None and self.restore != RUNNING

    def start_plan(self):
        if not self.can_plan():
            raise RuntimeError('plan not allowed now')
        self.running, self.plan, self.plan_data = 'plan', RUNNING, None

    def plan_finished(self, plan, error='', safe=True):
        self.running = None
        if plan is not None and safe:
            self.plan, self.plan_data = OK, plan
        else:
            self.plan, self.plan_data = FAILED, plan
            self.message = 'Plan yapılamadı: ' + (error or 'güvenli olmayan plan')

    def can_restore(self):
        return self.running is None and self.plan == OK and bool(self.port) and self.restore != RUNNING

    def start_restore(self):
        if not self.can_restore():
            raise RuntimeError('restore not allowed now')
        self.running, self.restore, self.plan = 'restore', RUNNING, 'used'

    def restore_finished(self, result):
        self.running = None
        self.result = result
        self.restore = OK if result.passed else FAILED
        if result.passed:
            self.message = 'Geri yükleme tamam: VERIFIED, PASS.'
        elif result.writes_started:
            self.message = ('Geri yükleme yazma başladıktan sonra durdu: otomatik tekrar yok. Telsizi kapatmayın; '
                            'aynı yedekle geri yüklemeyi yeniden başlatabilirsiniz (her şeyi yedekten yazar).')
            self.plan = IDLE
        else:
            self.message = 'Hiçbir şey yazılmadı' + (' (telsiz zamanında açılmadı)' if result.timeout else '') + '.'
            self.plan = IDLE
