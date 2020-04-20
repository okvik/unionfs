</$objtype/mkfile

BIN=$home/bin/$objtype
MAN=/sys/man/4
TARG=unionfs

HFILES=\
	unionfs.h
OFILES=\
	util.$O\
	unionfs.$O

</sys/src/cmd/mkone

install:V: man

uninstall:V:
	rm -f $BIN/$TARG
	rm -f $MAN/$TARG
