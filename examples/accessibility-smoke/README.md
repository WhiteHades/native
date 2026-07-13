# Accessibility smoke

A deterministic native-only fixture for installed AT-SPI and UIA probes. Run its headless suite from this directory after building the repository CLI:

```bash
NATIVE_SDK_PATH=../.. ../../zig-out/bin/native test -Dplatform=null
```

The semantic tree includes `Lesson list` with seven materialized rows from a 1,000-row logical collection. `Lesson 42` reports position 42 of 1,000 and is partially clipped at the top; `Lesson 40` is materialized but fully above the viewport. Stable controls expose their results as named text nodes (`Count result: 0`, `Study mode result: off`, and so on). `Remove transient` removes `Transient target` for retained-provider teardown checks.
