#!/usr/bin/env python3
#
# Example script for monitoring remote PTP nodes using JSON logging.
#
# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.
#
# Usage: python2 monitoring_console.py <remote-monitor.jsonl>
#
# Relevant options on monitored nodes:
#
# [ptp]
# mon_rx_sync_computed_data
# mon_slave_status
#
# Relevant options on remote monitor (can also include the above
# options for self-monitoring):
#
# [general]
# json_remote_monitor <jsonl-output-path>
#
# [ptp]
# remote_monitor

import curses
import time
import json
import sys
from datetime import datetime

#############
# Global Variables
#############

nodes = {}
events = {}
event_shortcuts = []
details_selected = None
details_type = "uncleared events"

#############
# Constants
#############

COL_SYNCED=1
COL_ALARMED=2
COL_LINK=3

#############
# Functions to process JSON records and save in appropriate local data structures,
# generally replacing the previous value attached to a node in the nodes dictionary.
#############

def process_node(record):
    if not (record['port-id'] in nodes):
        nodes[record['port-id']] = record

# Process rx event with timestamp data
#   this example monitor does not actually use the timestamp, which is more
#   useful for debugging than monitoring
def process_rx_event_ts(record):
    node = nodes[record['node']]
    node['last_rx_event_ts'] = record

# Process rx event with computed data
def process_rx_event_comp(record):
    node = nodes[record['node']]
    node['last_rx_event_comp'] = record

def process_tx_event(record):
    node = nodes[record['node']]
    node['last_tx_event'] = record

def add_event(record, name, description = None):
    event = (record['node'], name)
    if event in events:
        events[event]['instances'].append(record)
    else:
        events[event] = { 'time': record['monitor-timestamp'], 'description': description, 'instances': [record] }
    curses.beep()
    curses.flash()

def process_slave_status(record):
    node = nodes[record['node']]
    if not('last_slave_status' in node):
        node['last_slave_status'] = record
    else:
        # In additon to saving the latest slave status information we
        # work out if this should trigger an event, e.g. for an important
        # change of state like an alarm.

        previous = node['last_slave_status']
        node['last_slave_status'] = record

        # Record bond-changed events
        if (record['bond-changed']):
            add_event(record, "bond-changed")

        # Raise an event if an alarm is set when none were before
        all_alarms = record['msg-alarms'] + record['alarms']
        if (len(all_alarms) != 0 and
            (len(previous['msg-alarms']) == 0 and len(previous['alarms']) == 0)):
            add_event(record, "alarmed", ','.join(all_alarms))

        # Raise an event if there is a transition from the slave state
        if (record['state'] != 'SLAVE' and previous['state'] == 'SLAVE'):
            add_event(record, "left-slave-state")

        # Raise an event if an in-sync node goes out-of-sync
        if (not (record['in-sync']) and previous['in-sync']):
            add_event(record, "lost-sync")

def process_line(line):
    obj = json.loads(line)
    if 'node' in obj:
        process_node(obj['node'])
    if 'rx-event' in obj:
        evt = obj['rx-event']
        if ('offset-from-master' in evt or
            'mean-path-delay' in evt):
            process_rx_event_comp(evt)
        if ('sync-ingress-timestamp' in evt):
            process_rx_event_ts(evt)
    if 'tx-event' in obj:
        process_tx_event(obj['tx-event'])
    if 'slave-status' in obj:
        process_slave_status(obj['slave-status'])

#############
# Functions for rendering the console.
#############

def draw_window(window, name):
    (my, mx) = window.getmaxyx()
    window.erase()
    window.border(0, 0, 0, 0, 0, 0, 0, 0)
    window.addnstr(0, 2, " %s " % name, mx - 4)
    window.refresh()

def draw_details_window():
    if (details_selected != None):
        node, typ = details_selected
        draw_window(details_win, "detail: %s: %s: %s" % (details_type, typ, node))

def fullupdate_display():
    draw_window(nodes_win, "nodes")
    draw_window(alarms_win, "current alarms")
    draw_window(events_win, "uncleared events")
    draw_details_window()
    update_display()

def update_window(win, pad):
    (pad_rows, pad_cols) = pad.getmaxyx()
    (win_rows, win_cols) = win.getmaxyx()
    pad.overwrite(win, 0, 0, 1, 1,
                  min(win_rows - 2, pad_rows),
                  min(win_cols - 2, pad_cols))
    win.refresh()

#############
# Helper function to work out how long ago a textual date/time was and show
# the difference in reasonable units.
#############

def time_delta(timestr):
    t = datetime.strptime(timestr, "%Y-%m-%d %H:%M:%S.%f")
    td = (datetime.utcnow() - t).seconds
    if td == 0:
        return "now"
    if td < 61:
        return "+%ds" % td
    td = td / 60
    if td < 61:
        return "+%dm" % td
    td = td / 60
    if td < 25:
        return "+%dh" % td
    td = td / 24
    return "+%dd" %td

#############
# Function to update the nodes window which shows the latest information for each
# known remote node.
#############

def update_nodes():
    nlist = sorted(nodes, key=lambda x: nodes[x]['port-id'])
    nodes_pad.erase()
    nodes_pad.addstr("%-25s | %-6s | %13s | %13s | %9s | %3s | %3s | %4s | %7s | %7s | %-44s\n" % ("port", "domain", "offset", "mpd", "state", "sel", "syn", "alrm", "last rx", "last st", "address"))
    for port in nlist:
        node = nodes[port]
        offset = float('nan')
        mpd = float('nan')
        last_rx = "never"
        if 'last_rx_event_comp' in node:
            rx_event = node['last_rx_event_comp']
            # offset and mpd are only in the event if reported valid
            if 'offset-from-master' in rx_event:
                offset = rx_event['offset-from-master']
            if 'mean-path-delay' in rx_event:
                mpd = rx_event['mean-path-delay']
            last_rx = time_delta(rx_event['monitor-timestamp'])
        state = ""
        sel_str = ""
        syn_str = ""
        alrm_str = ""
        last_slave_status = "never"
        selected = None
        in_sync = None
        alarmed = None
        if 'last_slave_status' in node:
            slave_status = node['last_slave_status']
            state = slave_status['state']
            selected = slave_status['selected']
            in_sync = slave_status['in-sync']
            alarmed = len(slave_status['msg-alarms']) != 0 or len(slave_status['alarms']) != 0
            last_slave_status = time_delta(slave_status['monitor-timestamp'])
            if selected:
                sel_str = 'Sel'
            else:
                sel_str = '---'
            if in_sync:
                syn_str = 'Syn'
            else:
                syn_str = '---'
            if alarmed:
                alrm_str = 'ALRM'
            else:
                alrm_str = '----'

        colour = curses.color_pair(0)
        if (selected and in_sync):
            colour = curses.color_pair(COL_SYNCED)
        if (alarmed):
            colour = curses.color_pair(COL_ALARMED)

        nodes_pad.addstr("%-25s | %-6d | %13.03f | %13.03f | %9s | %3s | %3s | %4s | %7s | %7s | %-44s\n" % (
            node['port-id'], node['domain'],
            offset, mpd,
            state,
            sel_str, syn_str, alrm_str,
            last_rx,
            last_slave_status,
            node['address']), colour)

    update_window(nodes_win, nodes_pad)

#############
# Function to update the alarms window with any current alarms.
#############

def update_alarms():
    nlist = sorted(nodes, key=lambda x: nodes[x]['port-id'])
    alarms = {}
    alarms_pad.erase()
    for port in nlist:
        node = nodes[port]
        if 'last_slave_status' in node:
            slave_status = node['last_slave_status']
            for alarm in slave_status['msg-alarms'] + slave_status['alarms']:
                if not alarm in alarms:
                    alarms[alarm] = []
                lst = alarms[alarm]
                lst.append(node)
    for alarm in alarms:
        alarms_pad.addstr("%s\n" % alarm)
        for node in alarms[alarm]:
            alarms_pad.addstr("    %s\n" % node['port-id'])

    update_window(alarms_win, alarms_pad)

#############
# Function to update the uncleared events window with all events that have
# not explicitly been cleared by the user.
#############

def update_events():
    elist = sorted(events, key=lambda x: x[1])
    prevtyp = ''
    global event_shortcuts
    event_shortcuts = []
    shortcut = 0
    events_pad.erase()
    for event in elist:
        node, typ = event
        data = events[event]
        if typ != prevtyp:
            events_pad.addstr("%s\n" % typ)

        colour = curses.color_pair(0)
        if (typ == 'alarmed'):
            colour = curses.color_pair(COL_ALARMED)

        description = ""
        if (data['description'] != None):
            description = " (%s)" % data['description']
        if shortcut < 10:
            events_pad.addstr(" ")
            events_pad.addstr(("%d" % shortcut), curses.A_UNDERLINE | curses.color_pair(COL_LINK))
            event_shortcuts.append(event)
            shortcut = shortcut + 1
        else:
            events_pad.addstr("  ")
        events_pad.addstr("%24s %s%s\n" % (node, time_delta(data['time']), description), colour)
        prevtyp = typ

    if len(elist) != 0:
        events_pad.addstr("\npress ")
        events_pad.addstr("c", curses.A_UNDERLINE | curses.color_pair(COL_LINK))
        events_pad.addstr(" to clear events\n")

    update_window(events_win, events_pad)

#############
# Function to update the details pop-up window.
#############

def update_details():
    draw_details_window()
    if (details_selected):
        update_window(details_win, details_pad)

def update_display():
    update_nodes()
    update_alarms()
    update_events()
    update_details()

def select_details(event):
    global details_selected
    details_selected = event

    details_pad.erase()
    if details_selected:
        e = events[details_selected]
        records = sorted(e['instances'], key=lambda x: x['monitor-timestamp'])
        details_pad.addstr("%27s | %9s | %s\n" %
                           ("time", "state", "alarms"))
        for r in records:
            details_pad.addstr("%27s | %9s | %s\n" %
                               (r['monitor-timestamp'], r['state'],
                                ' '.join(r['msg-alarms'] + r['alarms'])))
        details_pad.addstr("\n press ")
        details_pad.addstr("space", curses.A_UNDERLINE | curses.color_pair(COL_LINK))
        details_pad.addstr(" to clear this event\n")

    fullupdate_display()

#############
# Entry to the utility.
#############

if len(sys.argv) != 2:
    print("single argument must be supplied with the path to the JSON Lines remote monitoring log")
    sys.exit(1)

logfile = open(sys.argv[1], 'r')

scr = curses.initscr()
curses.start_color()
scr.nodelay(True)
curses.noecho()

curses.init_pair(COL_ALARMED, curses.COLOR_RED, curses.COLOR_BLACK)
curses.init_pair(COL_SYNCED, curses.COLOR_GREEN, curses.COLOR_BLACK)
curses.init_pair(COL_LINK, curses.COLOR_BLUE, curses.COLOR_BLACK)

smy,smx = scr.getmaxyx()

nodes_win = curses.newwin(int(smy/2), smx)
alarms_win = curses.newwin(smy - int(smy/2), int(smx/2), int(smy/2), 0)
events_win = curses.newwin(smy - int(smy/2), smx - int(smx/2), int(smy/2), int(smx/2))
details_win = curses.newwin(int(smy*3/4), int(smx*3/4), int(smy/8), int(smx/8))

nodes_pad = curses.newpad(512, 256)
alarms_pad = curses.newpad(512, 128)
events_pad = curses.newpad(512, 128)
details_pad = curses.newpad(512, 256)

# Inject a refresh request into the execution loop
curses.ungetch(12)

#############
# Simple main execution loop.
#
#   - checks for any keypresses
#        ^L   refresh
#        q    quit
#        c    clear events
#
#   - reads the remote monitoring JSON Lines log and
#     calls processing functions on them
#
#   - handles changes to the size of the terminal and
#     resizes the windows
#
#############

doquit = False
while not(doquit):
    # Handle user input
    ch = 0
    while ch != -1:
        ch = scr.getch()
        if ch == 12:
            fullupdate_display()
        if details_selected:
            if ch == ord(' '):
                del events[details_selected]
                select_details(None)
        else:
            if ch == ord('q'):
                doquit = True
            if ch == ord('c'):
                events = {}
                event_shortcuts = []
            if ch >= ord('0') and (ch - ord('0')) < len(event_shortcuts):
                select_details(event_shortcuts[ch - ord('0')])

    # Handle logging updates
    pos = logfile.tell()
    line = logfile.readline()
    if not line:
        update_display()
        time.sleep(0.1)
        logfile.seek(pos)
    else:
        process_line(line)

    # Handle terminal size changes
    smy2,smx2 = scr.getmaxyx()

    if (smy2 != smy or smx2 != smx):
        smy = smy2
        smx = smx2
        nodes_win.resize(int(smy/2), smx)
        alarms_win.resize(smy - int(smy/2), int(smx/2))
        events_win.resize(smy - int(smy/2), smx - int(smx/2))
        details_win.resize(int(smy*3/4), int(smx*3/4))
        alarms_win.mvwin(int(smy/2), 0)
        events_win.mvwin(int(smy/2), int(smx/2))
        details_win.mvwin(int(smy/8), int(smx/8))
        fullupdate_display()

# Clear up curses so that the terminal is nice again
curses.endwin()
