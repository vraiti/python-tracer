.PHONY: build clean

cpython/Makefile:
	cd cpython && ./configure --prefix=$(CURDIR)/build

build: cpython/Makefile
	$(MAKE) -C cpython -j$(shell nproc)
	$(MAKE) -C cpython altinstall
	cargo build --release --manifest-path postprocess/Cargo.toml
	cp postprocess/target/release/d3g-postprocess build/bin/

clean:
	test ! -f cpython/Makefile || $(MAKE) -C cpython clean
	cargo clean --manifest-path postprocess/Cargo.toml
	rm -rf build
