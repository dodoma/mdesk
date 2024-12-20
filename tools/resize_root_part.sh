#! /bin/bash
# -------------------------------------------------------------------
# resize_root_part.sh
#
# Script to resize root partition of a Pi OS image.
# Copyright 2021 Michael G. Spohn
# License: Attribution-ShareAlike 4.0 International
#                 (CC BY-SA 4.0)
# https://creativecommons.org/licenses/by-sa/4.0/legalcode
#
# Use this script at your own risk! No warranty of any kind provided.
#
# Contact information:
# mspohn@topmail.com
#
# Version 1.0 - 2021-06-26
# -------------------------------------------------------------------
# Size, in sectors, of additional space to add to root partition.
# Modify this value to suit your needs.
# Value should be divisible by 512.
ADD_SECTOR_COUNT=512000
echo "------------------------------------"
echo "   Script to resize / partition."
echo "       Written by M. Spohn"
echo "          Version 1.0"
echo "           2021-03-11"
echo " Use this script at your own risk."
echo " --------> No warranty <------------"
echo "        mspohn@topmail.com"
echo "------------------------------------"
echo
echo "Current / partition info:"
ROOT_PART_DEV=$(findmnt / -o source -n)
echo -e "\t/ partition is on: " $ROOT_PART_DEV
ROOT_PART_NAME=$(echo "$ROOT_PART_DEV" | cut -d "/" -f 3)
echo -e "\t/ partion name: " $ROOT_PART_NAME
ROOT_DEV_NAME=$(echo /sys/block/*/"${ROOT_PART_NAME}" | cut -d "/" -f 4)
#echo -e "\t/ device name: " $ROOT_DEV_NAME
ROOT_DEV="/dev/${ROOT_DEV_NAME}"
#echo -e "\t/ device: " $ROOT_DEV
ROOT_PART_NUM=$(cat "/sys/block/${ROOT_DEV_NAME}/${ROOT_PART_NAME}/partition")
echo -e "\t/ partion number: " $ROOT_PART_NUM
PARTITION_TABLE=$(parted -m "$ROOT_DEV" unit s print | tr -d 's')
LAST_PART_NUM=$(echo "$PARTITION_TABLE" | tail -n 1 | cut -d ":" -f 1)
ROOT_PART_LINE=$(echo "$PARTITION_TABLE" | grep -e "^${ROOT_PART_NUM}:")
ROOT_PART_START=$(echo "$ROOT_PART_LINE" | cut -d ":" -f 2)
echo -e "\t/ partition start sector: " $ROOT_PART_START
ROOT_PART_END=$(echo "$ROOT_PART_LINE" | cut -d ":" -f 3)
echo -e "\t/ partition end sector: " $ROOT_PART_END
ROOT_DEV_SIZE=$(cat "/sys/block/${ROOT_DEV_NAME}/size")
ORI_PART_SIZE=$(( $ROOT_PART_END - $ROOT_PART_START + 1 ))
echo -e "\t/ partion size in sectors: $ORI_PART_SIZE"
TARGET_END=$(($ROOT_PART_END + $ADD_SECTOR_COUNT))
# echo "TARGET_END: " $TARGET_END
# Sanity checks.
if [ "$ROOT_PART_END" -gt "$TARGET_END" ]; then
    echo "Root partition runs past the end of device."
    return 1
fi
if [ "$TARGET_END" -gt "$ROOT_DEV_SIZE" ]; then
    echo "Not enough room on root partition to add $ADD_SECTOR_SCOUNT sectors."
    return 1
fi
echo
echo "Adding $ADD_SECTOR_COUNT sectors to / partition table entry..."
# Use parted to change the partition table.
echo "Changing root partion size..."
if ! parted -m "$ROOT_DEV" u s resizepart "$ROOT_PART_NUM" "$TARGET_END"; then
    echo "Root partition resize failed."
    return 1
fi
# Use resize2fs to physically change partition size.
echo "Physically changing / partion layout..."
resize2fs $ROOT_PART_DEV
if [ $? -ne 0 ]; then
   echo "Error resizing root partion."
   return 1
fi
echo
echo "/ partition resize complete."
echo
ORI_ROOT_SIZE_BYTES=$(($ORI_PART_SIZE * 512))
NEW_ROOT_SIZE_SECTORS=$(($ORI_PART_SIZE + $ADD_SECTOR_COUNT))
NEW_ROOT_SIZE_BYTES=$(($NEW_ROOT_SIZE_SECTORS * 512))
echo "/ partition size change:"
echo -e "\tOld / partition size: $ORI_PART_SIZE (sectors), $ORI_ROOT_SIZE_BYTES (bytes)."
echo -e "\tNew / partition size: $NEW_ROOT_SIZE_SECTORS (sectors), $NEW_ROOT_SIZE_BYTES (bytes)."
