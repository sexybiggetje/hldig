# Main Makefile for hl://Dig
# Copyright (c) 2017 The hl://Dig Group <https://andy5995.github.io/hldig/>
# For copyright details, see the file COPYING in your distribution
# or the GNU Library General Public License version 2 or later
# <http://www.gnu.org/copyleft/lgpl.html>

include $(top_srcdir)/Makefile.config

if TESTS
TESTDIR = test
endif

SUBDIRS = db htlib htword htcommon htnet htfuzzy htdb hldig hlsearch httools installdir include $(TESTDIR) po

EXTRA_DIST = config.rpath m4/ChangeLog  .version Makefile.config contrib ABOUT-NLS

ACLOCAL_AMFLAGS = -I m4

dist-hook:
	find $(distdir) -depth -name CVS -print | xargs rm -fr

#
# If --enable-tests is not specified, should remove
# the test/Makefile anyway
#
distclean-local:
	if test "X$(TESTDIR)" = "Xtest" ; \
	then \
		rm -f test/Makefile test/test_functions ; \
	fi

install-data-hook:
	@echo ""
	@echo "Installation done."
	@echo ""
	@echo "Before you can start searching, you will need to create a"
	@echo "search database.  A sample script to do this has been"
	@echo "installed as " $(DESTDIR)$(bindir)/rundig

distdir = $(PACKAGE)-$(VERSION)
