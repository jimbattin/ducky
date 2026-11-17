BUN ?= bun
PORT ?= 3000
STAMP = node_modules/.installed
TOOLS_TS = $(wildcard ../tools/*.ts)

$(STAMP): package.json bun.lock
	$(BUN) install --frozen-lockfile
	touch $@

serve: $(STAMP)
	PORT=$(PORT) $(BUN) run server.ts

html: dist/editor.html

dist/editor.html: $(STAMP) build.ts $(shell find src -type f)
	$(BUN) run build.ts

test: $(STAMP)
	$(BUN) test

check: $(STAMP)
	$(BUN) x tsc --noEmit
	$(BUN) x biome check . $(TOOLS_TS)

fmt: $(STAMP)
	$(BUN) x biome check --write . $(TOOLS_TS)

clean:
	rm -rf dist

.PHONY: serve html test check fmt clean
