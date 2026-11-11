# Product Specifications

`aquarium_controller.json` is the manufacturer-maintained canonical Product Specification for this firmware target.

Edit this JSON file when changing product identity, resources, component declarations, public capabilities, mappings, or configuration schema. Do not edit generated C++ headers. The obsolete source-controlled `include/product/prakiva_product_specification.h` has been removed; the only header with that name is generated under `.pio/build/<environment>/generated_include/product/`.

During each PlatformIO build, `scripts/embed_product_spec.py` embeds the exact UTF-8 bytes of this file into a generated header. The build script uses byte-oriented file I/O so Windows newline conversion cannot silently change the embedded specification or its checksum.

The active product is `prakiva_aquarium_rev1` (see `aquarium_controller.json`).
The runtime registers four platform driver factories: `DummySensorDriver`,
`NullActuatorDriver`, `PwmActuatorDriver`, and `BinaryActuatorDriver`.
GPIO bindings are supplied by the product specification, not hardcoded in the scheduler.
