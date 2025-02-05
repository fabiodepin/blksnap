#!/bin/bash -e
#
# SPDX-License-Identifier: GPL-2.0+


if [ -z $1 ]
then
	DIFF_STORAGE_DIR=${HOME}
else
	DIFF_STORAGE_DIR=$1
fi
echo "Diff storage directory ${DIFF_STORAGE_DIR}"

. ./functions.sh
. ./blksnap.sh

echo "---"
echo "Simple test start"

modprobe blksnap
sleep 2s

# check module is ready
blksnap_version

TESTDIR=/tmp/blksnap-test
rm -rf ${TESTDIR}
mkdir -p ${TESTDIR}

MPDIR=/mnt/blksnap-test
rm -rf ${MPDIR}
mkdir -p ${MPDIR}


# create first device
IMAGEFILE_1=${TESTDIR}/simple_1.img
imagefile_make ${IMAGEFILE_1} 64
echo "new image file ${IMAGEFILE_1}"

DEVICE_1=$(loop_device_attach ${IMAGEFILE_1})
echo "new device ${DEVICE_1}"

MOUNTPOINT_1=${MPDIR}/simple_1
mkdir -p ${MOUNTPOINT_1}
mount ${DEVICE_1} ${MOUNTPOINT_1}

# create second device
IMAGEFILE_2=${TESTDIR}/simple_2.img
imagefile_make ${IMAGEFILE_2} 128
echo "new image file ${IMAGEFILE_2}"

DEVICE_2=$(loop_device_attach ${IMAGEFILE_2})
echo "new device ${DEVICE_2}"

MOUNTPOINT_2=${MPDIR}/simple_2
mkdir -p ${MOUNTPOINT_2}
mount ${DEVICE_2} ${MOUNTPOINT_2}

generate_files ${MOUNTPOINT_1} "before" 9
drop_cache

echo "Block device prepared, press ..."
#read -n 1

blksnap_snapshot_create "${DEVICE_1} ${DEVICE_2}"

DIFF_STORAGE=${DIFF_STORAGE_DIR}/diff_storage0
rm -f ${DIFF_STORAGE}
fallocate --length 1GiB ${DIFF_STORAGE}
blksnap_snapshot_append ${DIFF_STORAGE}

blksnap_snapshot_take

echo "Snapshot was token, press ..."
read -n 1

blksnap_snapshot_collect_all

echo "Write to original"
#echo "Write something" > ${MOUNTPOINT_1}/something.txt
generate_files ${MOUNTPOINT_1} "after" 3
drop_cache

check_files ${MOUNTPOINT_1}

echo "Check snapshots"
DEVICE_IMAGE_1=$(blksnap_get_image ${DEVICE_1})
IMAGE_1=${TESTDIR}/image0
mkdir -p ${IMAGE_1}
mount ${DEVICE_IMAGE_1} ${IMAGE_1}
check_files ${IMAGE_1}

echo "Write to snapshot"
generate_files ${IMAGE_1} "snapshot" 3

drop_cache
umount ${DEVICE_IMAGE_1}
mount ${DEVICE_IMAGE_1} ${IMAGE_1}

check_files ${IMAGE_1}

umount ${IMAGE_1}

blksnap_snapshot_destroy

echo "Destroy snapshot, press ..."
#read -n 1

rm ${DIFF_STORAGE}

drop_cache
umount ${DEVICE_1}
mount ${DEVICE_1} ${MOUNTPOINT_1}

check_files ${MOUNTPOINT_1}

echo "Destroy second device"
umount ${MOUNTPOINT_2}
loop_device_detach ${DEVICE_2}
imagefile_cleanup ${IMAGEFILE_2}

echo "Destroy first device"
umount ${MOUNTPOINT_1}
loop_device_detach ${DEVICE_1}
imagefile_cleanup ${IMAGEFILE_1}

echo "Tracking device info:"
blksnap_tracker_collect

echo "Unload module"
modprobe -r blksnap

echo "Simple test finish"
echo "---"
