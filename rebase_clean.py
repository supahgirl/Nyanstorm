#!/usr/bin/env python3
"""Clean rebase — strip diagnostics + AO fix only, no French handling."""
import subprocess, sys

BAD = "22e4479b26"
FIX = "4c67ac18ca"
PARENT = "ef645ed6c7"
ORIG_TIP = "b9f36c009a"

def run(cmd, okfail=False):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if r.returncode != 0 and not okfail:
        print(f"FAIL: {cmd}\n{r.stderr.strip()}")
        sys.exit(1)
    return r.stdout.strip()

commits = run(f"git log --reverse --format=%H {PARENT}..{ORIG_TIP}").split('\n')
commits = [c for c in commits if c]
print(f"Rebuilding {len(commits)} commits...")

run(f"git checkout Nyanstorm")
run(f"git reset --hard {PARENT}")

for i, commit in enumerate(commits):
    short = commit[:8]
    msg = run(f"git log --format=%s -1 {commit}")
    
    if commit.startswith(BAD):
        run(f"git cherry-pick -n {commit}")
        for f in run("git diff --cached --name-only -- '*.xml'").split('\n'):
            if f.strip():
                run(f"git checkout HEAD -- {f.strip()}", okfail=True)
        for cpp in ['indra/newview/fsradarmenu.cpp', 'indra/newview/fsradarmenu.h',
                     'indra/newview/fsparticipantlist.cpp', 'indra/newview/llpanelpeoplemenus.cpp']:
            try:
                with open(cpp) as fh: lines = fh.readlines()
                with open(cpp, 'w') as fh:
                    for line in lines:
                        if any(w in line for w in ['DumpActiveMotions','dumpActiveMotions']): continue
                        if 'animation_explorer' in line and 'showInstance' in line: continue
                        fh.write(line)
            except FileNotFoundError: pass
        run("git add -A")
        run(f'git commit -m "{msg}"')
    
    elif commit.startswith(FIX):
        run(f"git cherry-pick -n {commit}")
        for f in run("git diff --cached --name-only").split('\n'):
            if f.strip() and f.strip() != 'indra/newview/ao.cpp':
                run(f"git reset HEAD -- {f.strip()}", okfail=True)
                run(f"git checkout -- {f.strip()}", okfail=True)
        run("git add -A")
        run(f'git commit -m "fix: AO animation coloring on panel open"')
    
    else:
        run(f"git cherry-pick {commit}")

print(f"DONE. {len(commits)} commits. Run filter-branch on your machine for French.")
