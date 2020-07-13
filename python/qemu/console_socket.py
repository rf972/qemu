"""
QEMU Console Socket Module:

This python module implements a ConsoleSocket object,
which can drain a socket and optionally dump the bytes to file.
"""
# Copyright 2020 Linaro
#
# Authors:
#  Robert Foley <robert.foley@linaro.org>
#
# This code is licensed under the GPL version 2 or later.  See
# the COPYING file in the top-level directory.
#

import socket
import threading
from collections import deque
import time


class ConsoleSocket(socket.socket):
    """
    ConsoleSocket represents a socket attached to a char device.

    Optionally (if drain==True), drains the socket and places the bytes
    into an in memory buffer for later processing.

    Optionally a file path can be passed in and we will also
    dump the characters to this file for debug.
    """
    def __init__(self, address, file=None, drain=False):
        self._recv_timeout_sec = 300
        self._buffer = deque()
        self._drain_thread = None
        socket.socket.__init__(self, socket.AF_UNIX, socket.SOCK_STREAM)
        self.connect(address)
        self._drain = drain
        self._logfile = None
        if file:
            self._logfile = open(file, "w")
        self._open = True
        if drain:
            self._thread_start()

    def _drain_fn(self, sleep_time_s=0.5):
        """Drains the socket and runs until the socket _open is False."""
        while self._open:
            try:
                self._drain_socket()
            except socket.timeout:
                # The socket is expected to timeout since we set a
                # short timeout to allow thread to exit when
                # self._open is set to False.
                time.sleep(sleep_time_s)
            except Exception as err:
                raise err

    def _thread_start(self):
        """Kick off a thread to drain the socket."""
        if self._drain_thread is not None:
            return
        # Configure socket to not block and timeout.
        # This allows our drain thread to not block
        # on recieve and exit smoothly.
        socket.socket.setblocking(self, 0)
        socket.socket.settimeout(self, 1)
        self._drain_thread = threading.Thread(target=self._drain_fn)
        self._drain_thread.daemon = True
        self._drain_thread.start()

    def close(self):
        """Close the base object and wait for the thread to terminate"""
        if self._open:
            self._open = False
            if self._drain and self._drain_thread is not None:
                thread, self._drain_thread = self._drain_thread, None
                thread.join()
            socket.socket.close(self)
            if self._logfile:
                self._logfile.close()
                self._logfile = None

    def _drain_socket(self):
        """process arriving characters into in memory _buffer"""
        data = socket.socket.recv(self, 1)
        # latin1 is needed since there are some chars
        # we are receiving that cannot be encoded to utf-8
        # such as 0xe2, 0x80, 0xA6.
        string = data.decode("latin1")
        if self._logfile:
            self._logfile.write("{}".format(string))
            self._logfile.flush()
        for c in string:
            self._buffer.extend(c)

    def recv(self, buffer_size=1):
        """Return chars from in memory buffer"""
        if not self._drain:
            # Not buffering the socket, pass thru to socket.
            return socket.socket.recv(self, buffer_size)
        start_time = time.time()
        while len(self._buffer) < buffer_size:
            time.sleep(0.1)
            elapsed_sec = time.time() - start_time
            if elapsed_sec > self._recv_timeout_sec:
                raise socket.timeout
        chars = ''.join([self._buffer.popleft() for i in range(buffer_size)])
        # We choose to use latin1 to remain consistent with
        # handle_read() and give back the same data as the user would
        # receive if they were reading directly from the
        # socket w/o our intervention.
        return chars.encode("latin1")

    def set_blocking(self):
        """Maintain compatibility with socket API"""

    def settimeout(self, seconds):
        """Set current timeout on recv"""
        self._recv_timeout_sec = seconds
        # Only allow changing timeout when not draining,
        # since when draining we control the timeout.
        if not self._drain:
            socket.socket.settimeout(self, seconds)
