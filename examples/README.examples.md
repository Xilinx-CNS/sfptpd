Examples
========

edit-chrony-cmdline
----------------------

Script conforming to the requirements of sfptpd's `control_script` option for
controlling the lifecycle of chronyd.

This script uses the same method as is built in to sfptpd of appending a
private block to chrony's environment file before restarting it. As such, this
method can be used outside of sfptpd in a compatible way,

e.g.

  /usr/libexec/sfptpd/edit-chrony-cmdline restore

This example script is packaged under /usr/libexec.

chrony_clockcontrol.py
----------------------

**DEPRECATED**

Script conforming to the requirements of sfptpd's `control_script` option for
controlling the lifecycle of chronyd.

This script uses the original technique, before sfptpd gained a built-in
solution, of saving a backup copy of chrony's environment file before editing
it and replacing it when necessary. This solution is less robust and cannot be
used outside of sfptpd in a compatible way to sfptpd's internal approach.

This example script is packaged under /usr/libexec.

sfptpdctl-related files
-----------------------

Example files for integrating sfptpd control over its control channel into user
automation.

sfptpd_json_parse.html
----------------------

Example web page for consuming and graphing JSON real time stats from sfptpd.

sfptpd_stats_collectd.py
------------------------

Example script for consuming sfptpd stats in collectd.

monitoring_console.py
---------------------

Example for how to consume PTP event monitoring data captured by the sfptpmon
script from PTP hosts implementing option 16.11 of IEEE1588-2019.
