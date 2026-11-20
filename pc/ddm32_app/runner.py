"""Running a tool as a subprocess and streaming its lines (QProcess), plus the one-radio-step-at-a-time lock."""
from PyQt5.QtCore import QObject, QProcess, QProcessEnvironment, pyqtSignal


class ProcessRunner(QObject):
    """One subprocess. line(str) per output line (stdout and stderr merged), finished(exit_code)"""
    line = pyqtSignal(str)
    finished = pyqtSignal(int)
    started = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.proc = None
        self._buf = b''
        self.program, self.args = None, []

    def is_running(self):
        return self.proc is not None and self.proc.state() != QProcess.NotRunning

    def start(self, program, args, cwd=None):
        if self.is_running():
            raise RuntimeError('already running')
        self.program, self.args = program, list(args)
        self._buf = b''
        p = QProcess(self)
        p.setProcessChannelMode(QProcess.MergedChannels)
        env = QProcessEnvironment.systemEnvironment()
        env.insert('PYTHONUNBUFFERED', '1')
        env.insert('PYTHONIOENCODING', 'utf-8')
        p.setProcessEnvironment(env)
        if cwd:
            p.setWorkingDirectory(str(cwd))
        p.readyReadStandardOutput.connect(self._read)
        p.finished.connect(self._done)
        p.errorOccurred.connect(self._error)
        self.proc = p
        p.start(program, [str(a) for a in args])
        self.started.emit()

    def _read(self):
        self._buf += bytes(self.proc.readAllStandardOutput())
        *lines, self._buf = self._buf.split(b'\n')
        for raw in lines:
            self.line.emit(raw.rstrip(b'\r').decode('utf-8', 'replace'))

    def _done(self, code, status):
        self._read()
        if self._buf:
            self.line.emit(self._buf.decode('utf-8', 'replace'))
            self._buf = b''
        self.finished.emit(code if status == QProcess.NormalExit else -1)

    def _error(self, err):
        if err == QProcess.FailedToStart:
            self.line.emit(f'Program başlatılamadı: {self.program}')
            self.finished.emit(-2)

    def kill(self):
        if self.is_running():
            self.proc.kill()


class RadioLock:
    """Only one tool may use the radio cable at a time (backup, plan, install, restore, health, live), and none before
    the disclaimer is accepted: gate() must be true for take() to succeed"""

    def __init__(self, gate=None):
        self.owner = None
        self.gate = gate or (lambda: True)

    def allowed(self):
        return bool(self.gate())

    def free_for(self, owner):
        """Whether owner may take the cable now (for enabling its button)"""
        return self.allowed() and self.owner in (None, owner)

    def take(self, owner):
        if not self.allowed():
            return False
        if self.owner is not None and self.owner != owner:
            return False
        self.owner = owner
        return True

    def release(self, owner):
        if self.owner == owner:
            self.owner = None
