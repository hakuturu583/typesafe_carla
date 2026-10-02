"""Binding generator: bindings/*.yaml -> C ABI declarations, C++ shim and Codon FFI.

Repetitive plumbing (a handle check, argument conversions and one LibCarla
call) is generated; everything else stays hand-written. See docs/bindgen.md.
"""
