EMCC    ?= emcc
BUILD   := build

CFLAGS  := -O1 -g
LDFLAGS := -sWASMFS \
           -sASYNCIFY=2 \
           -sJSPI_EXPORTS=['main','run_experiment'] \
           -lopfs.js \
           -sMODULARIZE=1 \
           -sEXPORT_NAME=createModule \
           -sFORCE_FILESYSTEM \
           -sEXPORTED_RUNTIME_METHODS=['FS','cwrap','ccall'] \
           -sEXPORTED_FUNCTIONS=['_main','_run_experiment'] \
           -sALLOW_MEMORY_GROWTH=1 \
           -Wno-experimental

SRC := main.c

PTHREADS_CFLAGS  := -O1 -g -pthread
PTHREADS_LDFLAGS := -sWASMFS \
                    -sFORCE_FILESYSTEM \
                    -lopfs.js \
                    -pthread \
                    -sPROXY_TO_PTHREAD \
                    -sMODULARIZE=1 \
                    -sEXPORT_NAME=createModule \
                    -sEXPORTED_RUNTIME_METHODS=['FS','cwrap','ccall'] \
                    -sEXPORTED_FUNCTIONS=['_main','_run_experiment'] \
                    -sALLOW_MEMORY_GROWTH=1

.PHONY: all pthreads clean serve

all: $(BUILD)/main.mjs $(BUILD)/index.html pthreads

$(BUILD)/main.mjs: $(SRC) | $(BUILD)
	$(EMCC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@

$(BUILD)/index.html: index.html | $(BUILD)
	cp index.html $(BUILD)/index.html

pthreads: $(BUILD)/main_pt.mjs $(BUILD)/index_pthreads.html

$(BUILD)/main_pt.mjs: main_pthreads.c | $(BUILD)
	$(EMCC) $(PTHREADS_CFLAGS) main_pthreads.c $(PTHREADS_LDFLAGS) -o $@

$(BUILD)/index_pthreads.html: index_pthreads.html | $(BUILD)
	cp index_pthreads.html $(BUILD)/index_pthreads.html

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

serve: all
	python3 serve.py 18090
