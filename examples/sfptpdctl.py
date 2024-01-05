# Python 3 example script (with Python 2 compat) for customising for
# application-specific control of sfptpd

# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

import socket
import sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.connect('/var/run/sfptpd-control-v1.sock')
s.send(' '.join(sys.argv[1:]).encode())
s.close()
