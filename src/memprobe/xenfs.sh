#!/bin/bash
#
# Development-time only helper script.
# Mount XENFS in a guest that does not have Xen installed.
#

xenfs_setup()
{
	[ -e "/proc/xen/capabilities" ] && return 0
	[ -d "/proc/xen" ] || return 1

	modprobe xenfs || return 1
	mount -t xenfs  xenfs  /proc/xen || return 1

	return 0
}

xenfs_setup
