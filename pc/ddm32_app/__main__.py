"""python -m ddm32_app (from pc/); --run-tool NAME ARGS runs one radio tool; --self-update is the update helper;
--update-check MARKER is how the self-update helper starts a new program (the marker proves it came up)."""
import sys


def main(argv=None):
    argv = list(sys.argv if argv is None else argv)
    from PyQt5.QtCore import QTimer
    from PyQt5.QtWidgets import QApplication

    from . import APP_NAME, __version__, selfupdate
    from .main_window import MainWindow
    marker = None
    if selfupdate.CHECK_ARG in argv:
        i = argv.index(selfupdate.CHECK_ARG)
        marker = argv[i + 1] if i + 1 < len(argv) else None
        del argv[i:i + 2]
    app = QApplication(argv)
    app.setApplicationName(APP_NAME)
    app.setStyle('Fusion')
    w = MainWindow()
    w.show()
    QTimer.singleShot(200, w.first_start)
    if marker:
        QTimer.singleShot(1500, lambda: selfupdate.write_marker(marker, __version__))
    return app.exec_()


if __name__ == '__main__':
    from .toolhost import main_from_argv
    code = main_from_argv(sys.argv)
    sys.exit(main() if code is None else code)
