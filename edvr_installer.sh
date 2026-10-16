#!/bin/sh

case $0 in
    */*) script_dir=${0%/*} ;;
    *) script_dir=. ;;
esac
case $script_dir in
    /*) ;;
    *) script_dir=./$script_dir ;;
esac
script_dir=$(CDPATH= cd "$script_dir" && pwd -P) || exit 1

if [ -f "$script_dir/edvr-installer-linux.py" ]; then
    wrapper=$script_dir/edvr-installer-linux.py
elif [ -f "$script_dir/tools/install_proton.py" ]; then
    wrapper=$script_dir/tools/install_proton.py
else
    printf '%s\n' 'EDVR: Python wrapper missing beside edvr_installer.sh or in tools/install_proton.py' >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'EDVR: python3 is required; install Python 3 and try again' >&2
    exit 1
fi

exec python3 "$wrapper" "$@"
