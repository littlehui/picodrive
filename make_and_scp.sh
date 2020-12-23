##make -f Makefile.rg350 clean
make -f Makefile clean
make -f Makefile
mv picodrive.elf picodrive/
make -f Makefile opk
scp -r ./picodrive/picodrive.opk root@10.1.1.2:/media/sdcard/apps/Picodrive_littlehui.opk
