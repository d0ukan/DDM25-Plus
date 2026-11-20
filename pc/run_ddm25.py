"""Entry script for PyInstaller (a package's __main__ cannot be the frozen entry: relative imports).

DDM25Plus.exe --run-tool NAME ARGS runs one bundled radio tool in this process (no Qt), --self-update runs the update
helper (no Qt), see ddm25_app/toolhost.py; anything else starts the window.
"""
import sys

if __name__ == '__main__':
    from ddm25_app.toolhost import main_from_argv
    code = main_from_argv(sys.argv)
    if code is not None:
        sys.exit(code)
    from ddm25_app.__main__ import main
    sys.exit(main())
