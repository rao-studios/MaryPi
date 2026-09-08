# MaryPi: two kits, each with its own Makefile.
#   make ravynos-build | ravynos-test | ravynos-app
#   make linux-build | linux-test | linux-image TARGET=vm | linux-vm | linux-flash DISK=disk4
ravynos-%:
	$(MAKE) -C ravynos $*

linux-%:
	$(MAKE) -C linux $*
