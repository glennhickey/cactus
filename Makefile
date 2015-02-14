# order is important, libraries first
modules = cPecan api setup blastLib caf bar blast normalisation phylogeny reference faces check pipeline progressive preprocessor hal dbTest externalTools



.PHONY: all %.all clean %.clean

all : ${modules:%=all.%}

all.%:
	cd $* && make all

clean:  ${modules:%=clean.%}
	rm -rf lib/*.h bin/*.dSYM

clean.%:
	cd $* && make clean
	
test: all
	python allTests.py