# Python 3 script

# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2012-2019 Xilinx, Inc.

# Before running this script, you may want to delete $FIFO (if it exists).
# When $FIFO exists, sfptpd will block until this script is running,
# so it is a good idea to start this script first.

# You will also want to customise which data is sent to collectd
# by modifying the collectd_reader() function near the end of this file.

# You can uncomment the 'print stat' line below and run this script
# from your shell to inspect the incoming data.

FIFO='/tmp/sfptpd_stats.jsonl'

import os
import sys
import stat
import errno
import json
import collectd
from queue import Queue, Empty
from threading import Thread

if not stat.S_ISFIFO(os.stat("/tmp/sfptpd_stats.jsonl").st_mode):
	print(FIFO, 'is not a FIFO! Aborting...')
	sys.exit(1)
try:
	os.mkfifo(FIFO)
except OSError as oe:
    if oe.errno != errno.EEXIST:
        raise

queue = Queue(0)

# FIFO reader thread, loops forever
def read_thread():
	while True:
		with open(FIFO) as fifo:
			data = ''
			while True:
				new_data = fifo.read(64)
				if len(new_data) == 0: break # FIFO was closed
				data += new_data
				if "\n" not in data: continue
				line = data[:data.index("\n")]
				data = data[data.index("\n")+1:]
				try:
					queue.put(json.loads(line))
				except ValueError:
					print('Invalid JSON received:', line)

def start_reader_thread():
	fifo_reader = Thread(target=read_thread)
	fifo_reader.daemon = True
	fifo_reader.start()

# If we're run from the shell just run in test mode
if __name__=="__main__":
	print('Shell detected, running in test mode (no collectd stuff)')
	print('Press CTRL-C to exit...')
	start_reader_thread()
	try:
		while True:
			try:
				stat = queue.get(timeout = 3)
				print("Got data from '%s', offset = %f" % (stat['instance'], stat['stats'].get('offset', float('nan'))))
				#print(stat) # Uncomment this to see the full objects
			except Empty:
				continue
	except KeyboardInterrupt:
		print("Terminating...")
		sys.exit(0)

### collectd stuff ###

#def collectd_configer(ObjConfiguration):
#    collectd.debug('Configuring Stuff')

def collectd_initer():
	collectd.debug('starting FIFO reader thread: %s' % FIFO)
	start_reader_thread()

def collectd_reader(input_data=None):
	try:
		while True:
			stat = queue.get_nowait() # Get next entry
			# Just track PTP offset to grandmaster. Adapt this to your needs
			if stat['instance'].startswith('ptp') and \
			   stat['clock-master']['name'] == 'gm' and \
			   stat['clock-slave']['name'].startswith('phc') and \
			   'offset' in stat['stats'].keys():
				metric = collectd.Values(type = 'gauge');
				metric.plugin = 'sfptpd'
				metric.dispatch(values = [ stat['stats']['offset'] ])
	except Empty:
		return # We've processed everything in the queue

#== Hook Callbacks, Order is important! ==#
#collectd.register_config(collectd_configer)
collectd.register_init(collectd_initer)
collectd.register_read(collectd_reader)

