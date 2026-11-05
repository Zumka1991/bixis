SUBDIRS = term

all install uninstall clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all install uninstall clean
