BUN ?= bun
PORT ?= 3000
STAMP = node_modules/.installed

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
	$(BUN) x biome check . ../tools/nam_convert.ts ../tools/ir_convert.ts ../tools/make_ir_fixture.ts ../tools/preset_bank.ts ../tools/make_preset_fixture.ts ../tools/pedal.ts ../tools/rawmidi.ts ../tools/slot_source.ts

fmt: $(STAMP)
	$(BUN) x biome check --write . ../tools/nam_convert.ts ../tools/ir_convert.ts ../tools/make_ir_fixture.ts ../tools/preset_bank.ts ../tools/make_preset_fixture.ts ../tools/pedal.ts ../tools/rawmidi.ts ../tools/slot_source.ts

clean:
	rm -rf dist

.PHONY: serve html test check fmt clean
