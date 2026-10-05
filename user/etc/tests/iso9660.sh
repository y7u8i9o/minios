# ISO 9660 test, run as: sh /etc/tests/iso9660.sh (cases iso9660 and
# iso9660_plain). The script finds the CD drive whose image contains the
# file isotest.txt, mounts it read only on /mnt/cd, and runs /bin/isotest,
# which prints the manifest that the post script compares with the host
# tree.
mkdir -p /mnt/cd
found=""
for d in sr0 sr1 sr2 sr3; do
    [ -e /dev/$d ] || continue
    if mount iso9660 $d /mnt/cd 2>/dev/null; then
        if [ -e /mnt/cd/isotest.txt ]; then
            found=$d
            break
        fi
        mount -u /mnt/cd
    fi
done
if [ -z "$found" ]; then
    echo "FAIL iso9660: no CD drive with the test image"
    exit 1
fi
echo "iso9660: test image on $found"
case "$(mount)" in
*"/mnt/cd type iso9660"*) ;;
*) echo "FAIL iso9660: not listed by mount" ;;
esac
/bin/isotest /mnt/cd || exit 1
mount -u /mnt/cd || echo "FAIL iso9660: unmount"
echo "iso9660: done"
