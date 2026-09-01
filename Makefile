.PHONY: build clean

cpython/Makefile:
	cd cpython && ./configure --prefix=$(CURDIR)/build

build: cpython/Makefile
	mkdir -p build
	$(MAKE) -C cpython -j$(shell nproc)
	$(MAKE) -C cpython altinstall
	cargo build --release --manifest-path postprocess/Cargo.toml
	cp postprocess/target/release/d3g-postprocess $(CURDIR)/build
	cp -r configs build

clean:
	test ! -f cpython/Makefile || $(MAKE) -C cpython distclean
	cargo clean --manifest-path postprocess/Cargo.toml
	rm -rf build
