#!/bin/bash
set -e
cd ~/localsend-repo
gh auth setup-git
git remote set-url origin https://github.com/Mrcoolfuyu/localsend.git
git push --force origin main 2>&1 | tail -4
echo "PUSH_DONE"
