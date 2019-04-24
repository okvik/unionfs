</$objtype/mkfile

TARG=unionfs
OFILES=$TARG.$O
BIN=$home/bin/$objtype

</sys/src/cmd/mkone

install:V: sysinstall

sysinstall:V:
	cp unionfs.4.man /sys/man/4/unionfs

uninstall:V:
	rm -f $BIN/$TARG
	rm -f /sys/man/4/$TARG
