#!/bin/bash
# clean_apps.sh -- kill flashorder-P replicas/clients by exact comm name
# (comm is truncated to 15 chars: "flashorder-P-ap")
pkill -9 -x flashorder-P-ap 2>/dev/null
pkill -9 -x flashorder-P-cl 2>/dev/null
pkill -9 -x evil-client 2>/dev/null
sleep 1
echo "remaining_listeners=$(ss -tln 2>/dev/null | grep -cE ':(100|200)[0-9][0-9]')"
