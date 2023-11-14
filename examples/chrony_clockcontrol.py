#!/usr/bin/python3
#
# Example Python 3 script with Python 2 compat for enabling/disabling chronyd
# control of the system clock
#
# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2022 Xilinx, Inc.
#
# Usage: python3 chrony_clockcontrol.py enable|disable|save|restore
#

from __future__ import print_function
from enum import Enum, auto
import re, sys, os, shlex, subprocess, tempfile

# Edit this if the EnvironmentFile is in a non-standard location
chronyconf = '/etc/sysconfig/chronyd'

class Operation(Enum):
    ENABLE = auto()
    DISABLE = auto()
    SAVE = auto()
    RESTORE = auto()
    RESTORENORESTART = auto()

def usage():
    print("Usage:\n{} {}".format(
            sys.argv[0],
            "|".join([op.name.lower() for op in Operation])),
        file=sys.stderr)
    exit(1)

def update(op):
    # Write updated config to a temp file:
    # Preserve OPTIONS other than '-x' and any other comments/config
    # For "clockcontrol disable" add one '-x' to the OPTIONS
    temp = tempfile.NamedTemporaryFile(mode='w', dir=os.path.dirname(chronyconf),
                                       delete=False)
    with temp as outfile:
        with open(chronyconf,'r') as infile:
            for line in infile:
                m = re.match('OPTIONS="(.*)"', line)
                if m:
                    oldoptions = shlex.split(m.group(1))
                    newoptions = [option for option in oldoptions if option != '-x']
                    if op == Operation.DISABLE:
                        newoptions.append('-x')
                    line='OPTIONS="' + ' '.join(newoptions) +'"'+"\n"
                print(line, end='', file=outfile)

    # Backup the original config (once only)
    command = "cp -n '" + chronyconf + "' '" + chronyconf + ".bak'"
    rc = subprocess.call(command, shell=True)

    if rc == 0:
        # Make the revised config the active one
        command = "mv -f '" + temp.name + "' '" + chronyconf + "'"
        rc = subprocess.call(command, shell=True)

    return rc

def restart():
    # Now try to restart chronyd with the new options
    rc = subprocess.call('systemctl restart chronyd.service', shell=True)

    if rc != 0:
        print('systemctl restart returned ', rc, file=sys.stderr)
        exit(2)

    return rc

def main():
    if len(sys.argv) !=2:
        usage()

    try:
        op = Operation[sys.argv[1].upper()]
    except:
        usage()

    if op == Operation.SAVE:
        command = "cp '{}' '{}.save.{:d}'".format(chronyconf, chronyconf, os.getppid())
        rc = subprocess.call(command, shell=True)

    elif op == Operation.RESTORE or op == Operation.RESTORENORESTART:
        command = "mv '{}.save.{:d}' '{}'".format(chronyconf, os.getppid(), chronyconf)
        rc = subprocess.call(command, shell=True)
        if rc == 0 and op == Operation.RESTORE:
            rc = restart()

    else:
        rc = update(op)
        if rc == 0:
            rc = restart()

    return rc

if __name__ == "__main__":
    sys.exit(main())
