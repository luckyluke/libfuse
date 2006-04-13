#! /bin/sh

do_test() {
    echo ""
    echo " *****************************************************"
    echo " * testing in $1/ ..."
    echo " ***"
    make -C $1 hello || {
	echo "FAILED to compile hello translator from $1/"
	exit 2
    }

    test -e node && settrans -fg node

    settrans -fca node $1/hello || {
	echo "FAILED to set translator $1/hello"
	exit 2
    }

    test -e node/hello || {
	echo "node/hello file missing, but should be there."
	exit 1
    }

    test `stat --format "%s" node/hello` -eq 13 || {
	echo "node/hello expected to be 13 bytes long."
	exit 1
    }

    grep -q "Hello World" node/hello || {
	echo "node/hello doesn't contain 'Hello World'"
	exit 1
    }

    echo "looks good."
    echo
}

# now let's get it on ...
for A in example*; do do_test $A || exit 1; done
