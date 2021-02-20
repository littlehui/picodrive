##make -f Maefile.rg350 clean
##make -f Makefile clean
make -f Makefile
make -f Makefile opk
#scp -r ./PicoDrive.opk root@10.1.1.2:/media/sdcard/simplemenu/emulators/PicoDrive_littlehui.opk
scp -r ./PicoDrive.opk root@10.1.1.2:/media/sdcard/apps/PicoDrive_littlehui.opk
#scp -r PicoDrive root@10.1.1.2:/media/sdcard/apps/PicoDrive
