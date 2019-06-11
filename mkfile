</$objtype/mkfile

TARG=unionfs
OFILES=$TARG.$O
BIN=$home/bin/$objtype
MAN=/sys/man/4

</sys/src/cmd/mkone

install:V: man

uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG
