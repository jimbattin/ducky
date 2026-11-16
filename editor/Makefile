BUN ?= bun
STAMP = node_modules/.installed

$(STAMP): package.json bun.lock
	$(BUN) install --frozen-lockfile
	touch $@

test: $(STAMP)
	$(BUN) test

check: $(STAMP)
	$(BUN) x tsc --noEmit
	$(BUN) x biome check . ../tools/nam_convert.ts ../tools/ir_convert.ts ../tools/make_ir_fixture.ts ../tools/preset_bank.ts ../tools/make_preset_fixture.ts

fmt: $(STAMP)
	$(BUN) x biome check --write . ../tools/nam_convert.ts ../tools/ir_convert.ts ../tools/make_ir_fixture.ts ../tools/preset_bank.ts ../tools/make_preset_fixture.ts

.PHONY: test check fmt
