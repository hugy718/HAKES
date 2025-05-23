SGX_SDK ?= /opt/intel/sgxsdk
SGX_MODE ?= HW
SGX_ARCH ?= x64
SGX_DEBUG ?= 0

INSTALL ?= install
INSTALL_PREFIX ?= ./install
INSTALL_LIB_DIR = $(INSTALL_PREFIX)/lib
INSTALL_INCLUDE_DIR = $(INSTALL_PREFIX)/include/search-worker
INSTALL_BIN_DIR = $(INSTALL_PREFIX)/bin

.PHONY: all install clean mrproper ref sgx_u sgx_t enclave_api

enclave_api:
	$(MAKE) -ef sgx_u.mk enclave_api SGX_MODE=$(SGX_MODE) SGX_DEBUG=$(SGX_DEBUG) Enclave_Search_Dirs="$(Enclave_Search_Dirs)"
	$(MAKE) -ef sgx_t.mk enclave_api SGX_MODE=$(SGX_MODE) SGX_DEBUG=$(SGX_DEBUG) Enclave_Search_Dirs="$(Enclave_Search_Dirs)"

no_sgx: 
	@echo "Creating build directory"
	mkdir -p src/no-sgx/index-build
	mkdir -p src/no-sgx/index-build/blas
	mkdir -p src/no-sgx/index-build/ext
	mkdir -p src/no-sgx/index-build/impl
	mkdir -p src/no-sgx/index-build/invlists
	mkdir -p src/no-sgx/index-build/utils
	$(MAKE) -ef no_sgx.mk all 

sgx_u:
	$(MAKE) -ef sgx_u.mk all SGX_MODE=$(SGX_MODE) SGX_DEBUG=$(SGX_DEBUG) Enclave_Search_Dirs="$(Enclave_Search_Dirs)"

sgx_t:
	$(MAKE) -ef sgx_t.mk all SGX_MODE=$(SGX_MODE) SGX_DEBUG=$(SGX_DEBUG) Enclave_Search_Dirs="$(Enclave_Search_Dirs)"

all: no_sgx sgx_u sgx_t

install:
	$(INSTALL) -d $(INSTALL_INCLUDE_DIR)
	$(INSTALL) -d $(INSTALL_INCLUDE_DIR)/common
	$(INSTALL) -d $(INSTALL_INCLUDE_DIR)/trusted
	$(INSTALL) -d $(INSTALL_INCLUDE_DIR)/untrusted
	$(INSTALL) -C -m 644 include/search-worker/common/* $(INSTALL_INCLUDE_DIR)/common
	$(INSTALL) -C -m 644 include/search-worker/trusted/*.edl $(INSTALL_INCLUDE_DIR)/trusted
	$(INSTALL) -C -m 644 include/search-worker/untrusted/* $(INSTALL_INCLUDE_DIR)/untrusted
	$(INSTALL) -d $(INSTALL_LIB_DIR)
	if [ -f *.signed.so ]; then $(INSTALL) -C -m 664 *.signed.so $(INSTALL_LIB_DIR); fi;
	if [ -f *.a ]; then $(INSTALL) -C -m 644 *.a $(INSTALL_LIB_DIR); fi
	$(INSTALL) -d $(INSTALL_BIN_DIR)
	$(INSTALL) -C -m 755 search_server* $(INSTALL_BIN_DIR)

clean:
	$(MAKE) -ef no_sgx.mk clean
	$(MAKE) -ef sgx_u.mk clean
	$(MAKE) -ef sgx_t.mk clean

mrproper: clean
	rm -rf ./install
