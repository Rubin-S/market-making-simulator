"""The pre-registered day split, shared by every research script (Addendum A7).
A script runs on DEV by default and on the HOLDOUT with --holdout, writing results/*_<tag>.csv.
The holdout is run ONCE: --holdout refuses to start unless freeze-v1 is an ancestor of HEAD and
sim/, research/, config/ have no uncommitted changes, so the holdout always runs frozen, committed code."""
import subprocess, sys

DEV = ['20260401', '20260501', '20260601', '20260701']
HOLDOUT = ['20260801', '20260901', '20261001']


def split():
    """-> (days, tag). tag is 'dev' or 'holdout'."""
    if '--holdout' not in sys.argv[1:]:
        return DEV, 'dev'
    frozen = subprocess.run(['git', 'merge-base', '--is-ancestor', 'freeze-v1', 'HEAD'],
                            capture_output=True).returncode == 0
    dirty = '\n'.join(l for l in subprocess.run(
        ['git', 'status', '--porcelain', '--untracked-files=no', '--', 'sim', 'research', 'config'],
        capture_output=True, text=True).stdout.splitlines() if not l.endswith('.pyc'))
    if not frozen:
        sys.exit('--holdout refused: tag freeze-v1 is not in this checkout (Step 9a first)')
    if dirty:
        sys.exit(f'--holdout refused: uncommitted changes in sim/, research/ or config/:\n{dirty}')
    return HOLDOUT, 'holdout'
