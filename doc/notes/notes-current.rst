.. Copyright (C) Internet Systems Consortium, Inc. ("ISC")
..
.. SPDX-License-Identifier: MPL-2.0
..
.. This Source Code Form is subject to the terms of the Mozilla Public
.. License, v. 2.0.  If a copy of the MPL was not distributed with this
.. file, you can obtain one at https://mozilla.org/MPL/2.0/.
..
.. See the COPYRIGHT file distributed with this work for additional
.. information regarding copyright ownership.

Notes for BIND 9.17.23
----------------------

Security Fixes
~~~~~~~~~~~~~~

- None.

Known Issues
~~~~~~~~~~~~

- None.

New Features
~~~~~~~~~~~~

- None.

Removed Features
~~~~~~~~~~~~~~~~

- The IPv6 sockets are now explicitly restricted to sending and receiving IPv6
  packets only.  This renders the ``dig`` option ``+mapped`` non-functioning and
  thus the option has been removed. :gl:`#3093`

Feature Changes
~~~~~~~~~~~~~~~

- The DLZ API has been updated: EDNS Client-Subnet (ECS) options sent
  by a client are now included in the client information sent to DLZ
  modules when processing queries. :gl:`#3082`

- Add DEBUG(1) level messages when starting and ending BIND 9 task exclusive mode
  that stops the normal DNS operation (f.e. for reconfiguration, interface
  scans, and other events that require exclusive access to a shared resources).
  :gl:`#3137`

Bug Fixes
~~~~~~~~~

- With libuv >= 1.37.0, the recvmmsg support would not be enabled in ``named``
  reducing the maximum query-response performance.  The recvmmsg support would
  be used only in libuv 1.35.0 and 1.36.0.  This has been fixed.  :gl:`#3095`

- A failed view configuration during a named reconfiguration procedure could
  cause inconsistencies in BIND internal structures, causing a crash or other
  unexpected errors.  This has been fixed.  :gl:`#3060`

- Restore logging "quota reached" message when accepting connection is over
  hard quota.  :gl:`#3125`

- Build errors were introduced in some DLZ modules due to an incomplete
  change in the previous release. This has been fixed. :gl:`#3111`

- An error in the processing of the ``blackhole`` ACL could cause some DNS
  requests sent by ``named`` to fail - for example, zone transfer requests
  and SOA refresh queries - if the destination address or prefix was
  specifically excluded from the ACL using ``!``, or if the ACL was set
  to ``none``.  ``blackhole`` worked correctly when it was left unset, or
  if only positive-match elements were included. This has now been fixed.
  :gl:`#3157`

- TCP connections could hang indefinitely if the TCP write buffers
  were full because of the other party not reading sent data.  This has
  been fixed by adding a "write" timer. Connections that are hung
  while writing will now time out after the ``tcp-idle-timeout`` period
  has elapsed. :gl:`#3132`
